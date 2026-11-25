// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NXP/Freescale LS1024A (Comcerto 2000) SPACC - AES-CTR only.
 *
 * Sixteen hardware contexts used as a ring, up to fifteen commands in flight
 * (slot 15 kept free as ring sentinel).  The engine reads the request
 * scatterlist and writes straight back into it.
 *
 * The final 64 bytes of every command are recomputed on the CPU after
 * dma_unmap: this silicon corrupts the last 8-24 bytes of a DMA write burst
 * under memory-bus contention, and the measured window fits inside 64 bytes
 * (four AES blocks).  The source tail is copied out before the command, so
 * in-place buffers work as well.
 *
 * The CTR counter is only 16 bits wide in hardware, so a chunk never crosses
 * that boundary; the remainder and short requests are finished in software.
 *
 * Copyright (C) 2026 mrrdmr
 */

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/skcipher.h>
#include <linux/atomic.h>
#include <linux/bottom_half.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define DRV_NAME			"ls1024a-spacc"

/* PDU wrapper at +0x0, SPACC core at +0x40000 */
#define SPACC_CRYPTO_BASE_OFFSET	0x00040000
#define SPACC_MIN_MMIO_SIZE		0x00048000

#define SPACC_REG_IRQ_EN		0x00000000
#define SPACC_REG_IRQ_STAT		0x00000004
#define SPACC_REG_IRQ_CTRL		0x00000008
#define SPACC_REG_FIFO_STAT		0x0000000C
#define SPACC_REG_SRC_PTR		0x00000020
#define SPACC_REG_DST_PTR		0x00000024
#define SPACC_REG_OFFSET		0x00000028
#define SPACC_REG_PRE_AAD_LEN		0x0000002C
#define SPACC_REG_POST_AAD_LEN		0x00000030
#define SPACC_REG_PROC_LEN		0x00000034
#define SPACC_REG_ICV_LEN		0x00000038
#define SPACC_REG_ICV_OFFSET		0x0000003C
#define SPACC_REG_IV_OFFSET		0x00000040
#define SPACC_REG_SW_ID			0x00000044
#define SPACC_REG_AUX_INFO		0x00000048
#define SPACC_REG_CTRL			0x0000004C
#define SPACC_REG_STAT_POP		0x00000050
#define SPACC_REG_STATUS		0x00000054
#define SPACC_REG_KEY_SZ		0x00000100
#define SPACC_REG_ID			0x00000180
#define SPACC_REG_CIPH_KEY_BASE		0x00004000

/* Status codes */
#define SPA_STATUS_OK			0
#define SPA_STATUS_MEMORY_ERROR		2
#define SPA_STATUS_BLOCK_ERROR		3
#define SPA_STATUS_SECURITY_ERROR	4
#define SPA_STATUS_RES_CODE_OFFSET	24
#define SPA_STATUS_RES_CODE_MASK	(0x7U << SPA_STATUS_RES_CODE_OFFSET)

/* IRQ control/status bits */
#define SPA_IRQ_EN_STAT_EN		BIT(4)
#define SPA_IRQ_EN_GLBL_EN		BIT(31)
#define SPA_IRQ_STAT_CMD		BIT(0)
#define SPA_IRQ_STAT_STAT_MASK		BIT(4)
#define SPA_IRQ_CTRL_STAT_CNT_OFFSET	16

/* FIFO status bits */
#define SPA_FIFO_STAT_EMPTY		BIT(31)
#define SPA_FIFO_CMD_FULL		BIT(7)

/* Key size register bits */
#define SPA_KEY_SZ_CTX_INDEX_OFFSET	8
#define SPA_KEY_SZ_CIPHER		BIT(31)

/* Control register bits */
#define SPA_CTRL_CIPH_ALG_AES		0x02U
#define SPA_CTRL_CIPH_MODE_CTR		(0x02U << 8)
#define SPA_CTRL_CTX_IDX		16
#define SPA_CTRL_ENCRYPT		BIT(24)

/* Engine limits */
/* DDT prefetcher stops at 16 entries: 15 data segments plus the terminator */
#define SPACC_MAX_DDT_ENTS		16
#define SPACC_MAX_SG_ENTS		(SPACC_MAX_DDT_ENTS - 1)
#define SPACC_RING_SIZE			16
#define SPACC_QUEUE_LEN			128
#define SPACC_CIPHER_PG_SZ		128
#define SPACC_CIPHER_IV_OFFSET		32

/*
 * PROC_LEN and DDT segment length are 16-bit fields, hence 65520.  A source of
 * discontiguous 4 KiB pages is additionally capped at 15 * 4096 by
 * spacc_build_chunk_sgl().
 */
#define SPACC_MAX_CHUNK_BYTES		round_down(SZ_64K - 1, AES_BLOCK_SIZE)

/* Below this a request is cheaper in software than as a command */
#define SPACC_MIN_HW_BYTES		8192

/* A short remainder of a running request goes to software, not to a command */
#define SPACC_MIN_TAIL_HW_BYTES		2048

/*
 * Erratum window: the last 8-24 bytes of a write burst can be corrupted under
 * memory contention.  Four AES blocks cover the observed maximum with margin,
 * and the CPU recomputes exactly these bytes after dma_unmap().
 */
#define SPACC_TAIL_FIX_BYTES		64

static_assert(SPACC_TAIL_FIX_BYTES % AES_BLOCK_SIZE == 0);
static_assert(SPACC_MAX_SG_ENTS + 1 <= SPACC_MAX_DDT_ENTS);
static_assert(SPACC_CIPHER_IV_OFFSET + AES_BLOCK_SIZE <= SPACC_CIPHER_PG_SZ);
static_assert(SPACC_CRYPTO_BASE_OFFSET + SPACC_REG_CIPH_KEY_BASE +
	      SPACC_RING_SIZE * SPACC_CIPHER_PG_SZ <= SPACC_MIN_MMIO_SIZE);

struct spacc_ddt {
	__le32 p;
	__le32 len;
};

struct spacc_ctx;
struct spacc_dev;

struct spacc_alg {
	struct skcipher_alg alg;
	struct spacc_dev *sdev;
};

struct spacc_hw_slot {
	struct skcipher_request *req;
	/*
	 * Private exact-length bounded SG lists: each in-flight chunk owns its
	 * own SG entries trimmed to exact chunk byte length so streaming DMA
	 * mappings of adjacent chunks never overlap.
	 */
	struct scatterlist src_sgl[SPACC_MAX_SG_ENTS];
	struct scatterlist dst_sgl[SPACC_MAX_SG_ENTS];
	struct spacc_ddt *src_ddt;
	struct spacc_ddt *dst_ddt;
	dma_addr_t src_ddt_dma;
	dma_addr_t dst_ddt_dma;
	unsigned int src_nents;
	unsigned int dst_nents;
	unsigned int len;
	unsigned int tail_len;
	bool in_place;
	bool mapped;
	int err;

	/* Context SRAM key cache metadata */
	const struct spacc_ctx *last_ctx;
	unsigned long last_key_gen;
	unsigned int last_keylen;

	u8 tail_src[SPACC_TAIL_FIX_BYTES] __aligned(4);
	u8 tail_iv[AES_BLOCK_SIZE] __aligned(4);
};

struct spacc_hw {
	struct device *dev;
	void __iomem *regs;
	void __iomem *pdu_regs;
	void __iomem *crypto_regs;
	void __iomem *cipher_ctx_base;
	int irq;
	struct clk *clk;
	struct reset_control *rst;
	struct spacc_ddt *ddt_pool;
	dma_addr_t ddt_pool_dma;

	spinlock_t q_lock;
	struct crypto_queue queue;
	struct list_head active_reqs;

	spinlock_t hw_lock;
	struct spacc_hw_slot ring[SPACC_RING_SIZE];
	unsigned int head;
	unsigned int tail;
	unsigned int complete_idx;
	unsigned int hw_inflight;
	unsigned long last_progress;
	bool watchdog_armed;
	bool resetting;
	bool shutting_down;
	bool hw_dead;

	struct workqueue_struct *wq;
	struct work_struct work;
	struct delayed_work watchdog;
};

struct spacc_dev {
	struct kref refcount;
	struct spacc_hw __rcu *hw;
	struct spacc_alg alg;
};

struct spacc_ctx {
	struct spacc_dev *sdev;
	struct crypto_sync_skcipher *fallback_tfm;
	struct crypto_aes_ctx aes_ctx;
	u8 key[AES_MAX_KEY_SIZE] __aligned(4);
	unsigned int keylen;
	unsigned long key_gen;
	struct mutex lock;
};

struct spacc_reqctx {
	struct list_head node;
	struct skcipher_request *req;
	unsigned int offset;
	unsigned int inflight_cmds;
	unsigned int sw_tail_len;
	bool serialize_chunks;
	int err;
	u8 iv[AES_BLOCK_SIZE] __aligned(4);
};

static atomic_long_t spacc_key_gen_seq = ATOMIC_LONG_INIT(0);

static void spacc_dev_release(struct kref *ref)
{
	struct spacc_dev *sdev = container_of(ref, struct spacc_dev, refcount);

	kfree(sdev);
}

/*
 * Context memory holds key and IV in native little-endian 32-bit words, byte 0
 * in the LSB of word 0.  Stores are relaxed; the caller orders them against
 * the CTRL write with a plain writel().
 */
static inline void spacc_memcpy_toio32(void __iomem *dst, const void *src,
				       unsigned int bytes)
{
	u32 __iomem *dst32 = dst;
	const u8 *src8 = src;
	unsigned int words = bytes / 4;
	unsigned int i;

	WARN_ON_ONCE(bytes % 4);

	for (i = 0; i < words; i++)
		writel_relaxed(get_unaligned_le32(src8 + i * 4), dst32 + i);
}

static int spacc_status_to_errno(u32 code)
{
	switch (code) {
	case SPA_STATUS_OK:
		return 0;
	case SPA_STATUS_MEMORY_ERROR:
		return -EFAULT;
	case SPA_STATUS_SECURITY_ERROR:
		return -EACCES;
	case SPA_STATUS_BLOCK_ERROR:
		return -EIO;
	default:
		return -EINVAL;
	}
}

static unsigned int spacc_build_chunk_sgl(struct scatterlist *orig_sgl,
					  unsigned int offset,
					  unsigned int max_bytes,
					  struct scatterlist *dst_sgl,
					  unsigned int max_ents,
					  unsigned int *out_nents)
{
	struct scatterlist *sg = orig_sgl;
	unsigned int total = 0, used = 0;

	while (sg && offset >= sg->length) {
		offset -= sg->length;
		sg = sg_next(sg);
	}

	sg_init_table(dst_sgl, max_ents);

	while (sg && used < max_ents && total < max_bytes) {
		unsigned int avail = sg->length - offset;
		unsigned int take = min_t(unsigned int, avail,
					  max_bytes - total);
		unsigned int page_off = sg->offset + offset;

		if (take) {
			/*
			 * Block layer / THP scatterlists can pack multi-page
			 * compound folios into a single sg entry with
			 * page_off >= PAGE_SIZE; advance the struct page
			 * pointer via nth_page() so page + offset stays
			 * normalized within a single base page.
			 */
			sg_set_page(&dst_sgl[used],
				    sg_page(sg) + (page_off >> PAGE_SHIFT),
				    take, offset_in_page(page_off));
			total += take;
			used++;
		}
		offset = 0;
		sg = sg_next(sg);
	}

	if (used > 0)
		sg_mark_end(&dst_sgl[used - 1]);

	*out_nents = used;
	return total;
}

static void spacc_trim_sgl(struct scatterlist *sgl, unsigned int *nents,
			   unsigned int new_len)
{
	unsigned int rem = new_len;
	unsigned int i;

	for (i = 0; i < *nents; i++) {
		if (rem == 0)
			break;
		if (sgl[i].length >= rem) {
			sgl[i].length = rem;
			sg_mark_end(&sgl[i]);
			*nents = i + 1;
			return;
		}
		rem -= sgl[i].length;
	}

	*nents = i;
	if (i > 0)
		sg_mark_end(&sgl[i - 1]);
}

static bool spacc_map_sg_to_ddt(struct spacc_hw *hw,
				struct scatterlist *sgl,
				unsigned int nbytes,
				unsigned int nents,
				enum dma_data_direction dir,
				struct spacc_ddt *ddt)
{
	struct scatterlist *cur;
	int mapped_ents, i, ddt_idx = 0;

	if (!nents || nents > SPACC_MAX_SG_ENTS)
		return false;

	mapped_ents = dma_map_sg(hw->dev, sgl, nents, dir);
	if (mapped_ents <= 0)
		return false;

	for_each_sg(sgl, cur, mapped_ents, i) {
		unsigned int len = min_t(unsigned int, sg_dma_len(cur), nbytes);

		if (!len)
			continue;
		if (ddt_idx >= SPACC_MAX_SG_ENTS) {
			dma_unmap_sg(hw->dev, sgl, nents, dir);
			return false;
		}
		ddt[ddt_idx].p = cpu_to_le32(lower_32_bits(sg_dma_address(cur)));
		ddt[ddt_idx].len = cpu_to_le32(len);
		ddt_idx++;
		nbytes -= len;
		if (nbytes == 0)
			break;
	}

	if (nbytes != 0) {
		dma_unmap_sg(hw->dev, sgl, nents, dir);
		return false;
	}

	ddt[ddt_idx].p = 0;
	ddt[ddt_idx].len = 0;

	/* Read back the terminator: drains the write path before CTRL starts. */
	(void)READ_ONCE(ddt[ddt_idx].len);
	dma_wmb();

	return true;
}

/* Full 128-bit big-endian counter increment, matching NIST SP 800-38A */
static void spacc_ctr_advance_blocks(u8 *iv, u64 blocks)
{
	u32 carry = 0;
	int i;

	for (i = 15; i >= 0 && (blocks || carry); i--) {
		u32 v = iv[i] + (u32)(blocks & 0xff) + carry;

		iv[i] = v & 0xff;
		carry = v >> 8;
		blocks >>= 8;
	}
}

static int spacc_sw_crypt(struct spacc_ctx *ctx, struct scatterlist *src,
			  struct scatterlist *dst, unsigned int len, u8 *iv,
			  u32 flags)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(subreq, ctx->fallback_tfm);
	int err;

	if (unlikely(!ctx->fallback_tfm))
		return -EINVAL;

	skcipher_request_set_sync_tfm(subreq, ctx->fallback_tfm);
	skcipher_request_set_callback(subreq,
				      flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				      NULL, NULL);
	skcipher_request_set_crypt(subreq, src, dst, len, iv);

	err = crypto_skcipher_encrypt(subreq);
	skcipher_request_zero(subreq);

	return err;
}

static void spacc_invalidate_all_contexts(struct spacc_hw *hw)
{
	int i;

	lockdep_assert_held(&hw->hw_lock);

	for (i = 0; i < SPACC_RING_SIZE; i++) {
		hw->ring[i].last_ctx = NULL;
		hw->ring[i].last_key_gen = 0;
		hw->ring[i].last_keylen = 0;
	}
}

static void spacc_enable_irqs(struct spacc_hw *hw)
{
	lockdep_assert_held(&hw->hw_lock);

	writel(SPA_IRQ_EN_STAT_EN | SPA_IRQ_EN_GLBL_EN,
	       hw->pdu_regs + SPACC_REG_IRQ_EN);
}

static void spacc_hw_init_regs(struct spacc_hw *hw, bool enable_irq)
{
	int limit = 64;

	lockdep_assert_held(&hw->hw_lock);

	writel(0, hw->pdu_regs + SPACC_REG_IRQ_EN);

	while (limit-- &&
	       !(readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		writel(~0U, hw->crypto_regs + SPACC_REG_STAT_POP);
		(void)readl(hw->crypto_regs + SPACC_REG_STATUS);
	}

	/* Constant zero fields in pure AES-CTR mode: program once on init */
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_OFFSET);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_PRE_AAD_LEN);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_POST_AAD_LEN);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_ICV_LEN);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_ICV_OFFSET);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_IV_OFFSET);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_AUX_INFO);

	writel(~0U, hw->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(~0U, hw->pdu_regs + SPACC_REG_IRQ_STAT);

	/* STAT_CNT fixed at 1: one status entry per interrupt */
	writel(1U << SPA_IRQ_CTRL_STAT_CNT_OFFSET,
	       hw->crypto_regs + SPACC_REG_IRQ_CTRL);

	if (enable_irq)
		spacc_enable_irqs(hw);
}

static irqreturn_t spacc_irq_handler(int irq, void *dev_id)
{
	struct spacc_hw *hw = dev_id;
	unsigned long flags;
	u32 stat, cstat, status, code;
	bool completed_any = false;

	spin_lock_irqsave(&hw->hw_lock, flags);

	stat = readl(hw->pdu_regs + SPACC_REG_IRQ_STAT);
	if (!(stat & (SPA_IRQ_STAT_STAT_MASK | SPA_IRQ_STAT_CMD))) {
		spin_unlock_irqrestore(&hw->hw_lock, flags);
		return IRQ_NONE;
	}

	cstat = readl(hw->crypto_regs + SPACC_REG_IRQ_STAT);
	if (cstat)
		writel(cstat, hw->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(stat, hw->pdu_regs + SPACC_REG_IRQ_STAT);

	while (hw->hw_inflight > 0 &&
	       !(readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		struct spacc_hw_slot *slot = &hw->ring[hw->complete_idx];

		writel(~0U, hw->crypto_regs + SPACC_REG_STAT_POP);
		status = readl(hw->crypto_regs + SPACC_REG_STATUS);
		code = (status & SPA_STATUS_RES_CODE_MASK) >>
		       SPA_STATUS_RES_CODE_OFFSET;
		if (code != SPA_STATUS_OK)
			dev_warn_ratelimited(hw->dev,
					     "SPACC: command failed, STATUS=0x%08x (code %u)\n",
					     status, code);

		slot->err = spacc_status_to_errno(code);
		hw->complete_idx = (hw->complete_idx + 1) % SPACC_RING_SIZE;
		hw->hw_inflight--;
		hw->last_progress = jiffies;
		completed_any = true;
	}

	spin_unlock_irqrestore(&hw->hw_lock, flags);

	if (completed_any && !READ_ONCE(hw->shutting_down))
		queue_work(hw->wq, &hw->work);

	return IRQ_HANDLED;
}

static void spacc_unmap_slot(struct spacc_hw *hw, struct spacc_hw_slot *slot)
{
	if (!slot->mapped)
		return;

	if (slot->in_place) {
		dma_unmap_sg(hw->dev, slot->src_sgl, slot->src_nents,
			     DMA_BIDIRECTIONAL);
	} else {
		dma_unmap_sg(hw->dev, slot->src_sgl, slot->src_nents,
			     DMA_TO_DEVICE);
		dma_unmap_sg(hw->dev, slot->dst_sgl, slot->dst_nents,
			     DMA_FROM_DEVICE);
	}
	slot->mapped = false;
}

static void spacc_clear_slot_secrets(struct spacc_hw_slot *slot)
{
	memzero_explicit(slot->tail_src, sizeof(slot->tail_src));
	memzero_explicit(slot->tail_iv, sizeof(slot->tail_iv));
	slot->tail_len = 0;
}

/*
 * Fast direct-AES CTR fixup for the command tail without allocating a
 * sync_skcipher request or walking the Crypto API dispatch layer.
 */
static void spacc_fix_tail_direct_aes(const struct crypto_aes_ctx *aes_ctx,
				      const u8 *src_buf,
				      struct scatterlist *dst_base,
				      unsigned int dst_offset,
				      unsigned int tail_len,
				      const u8 *start_iv)
{
	u8 ctr[AES_BLOCK_SIZE] __aligned(4);
	u8 stream[AES_BLOCK_SIZE] __aligned(4);
	u8 out_buf[SPACC_TAIL_FIX_BYTES] __aligned(4);
	unsigned int pos = 0;

	memcpy(ctr, start_iv, AES_BLOCK_SIZE);

	while (pos < tail_len) {
		unsigned int step = min_t(unsigned int, AES_BLOCK_SIZE,
					  tail_len - pos);
		unsigned int i;

		aes_encrypt(aes_ctx, stream, ctr);
		for (i = 0; i < step; i++)
			out_buf[pos + i] = src_buf[pos + i] ^ stream[i];

		pos += step;
		spacc_ctr_advance_blocks(ctr, 1);
	}

	scatterwalk_map_and_copy(out_buf, dst_base, dst_offset, tail_len, 1);

	memzero_explicit(ctr, sizeof(ctr));
	memzero_explicit(stream, sizeof(stream));
	memzero_explicit(out_buf, sizeof(out_buf));
}

/*
 * Recompute the command tail on the CPU.  The engine's last write burst may
 * have lost or scrambled 8-24 bytes; the source bytes were saved before the
 * command, so this works for in-place buffers too.
 */
static void spacc_fix_tail(struct spacc_ctx *ctx, struct spacc_hw_slot *slot)
{
	struct scatterlist *dst_base;

	if (!slot->tail_len)
		return;

	dst_base = slot->in_place ? slot->src_sgl : slot->dst_sgl;
	spacc_fix_tail_direct_aes(&ctx->aes_ctx, slot->tail_src, dst_base,
				  slot->len - slot->tail_len, slot->tail_len,
				  slot->tail_iv);
}

static void spacc_finish_request(struct skcipher_request *req, int err)
{
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);

	if (!list_empty(&rctx->node))
		list_del_init(&rctx->node);
	memzero_explicit(rctx->iv, sizeof(rctx->iv));
	local_bh_disable();
	skcipher_request_complete(req, err);
	local_bh_enable();
}

static void spacc_reap_completed(struct spacc_hw *hw)
{
	unsigned long flags;
	unsigned int comp_idx;

	spin_lock_irqsave(&hw->hw_lock, flags);
	comp_idx = hw->complete_idx;
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	while (hw->tail != comp_idx) {
		struct spacc_hw_slot *slot = &hw->ring[hw->tail];
		struct skcipher_request *req = slot->req;
		struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
		struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
		struct spacc_reqctx *rctx = skcipher_request_ctx(req);
		int slot_err = slot->err;

		spacc_unmap_slot(hw, slot);

		if (slot_err && !rctx->err)
			rctx->err = slot_err;

		if (!rctx->err)
			spacc_fix_tail(ctx, slot);

		spacc_clear_slot_secrets(slot);

		rctx->inflight_cmds--;
		slot->req = NULL;
		hw->tail = (hw->tail + 1) % SPACC_RING_SIZE;

		if (rctx->inflight_cmds == 0 &&
		    (rctx->err || rctx->sw_tail_len > 0 ||
		     rctx->offset == req->cryptlen)) {
			if (!rctx->err && rctx->sw_tail_len > 0) {
				struct scatterlist sc[2], dc[2];
				struct scatterlist *s, *d;

				s = scatterwalk_ffwd(sc, req->src, rctx->offset);
				d = (req->src == req->dst) ? s :
				    scatterwalk_ffwd(dc, req->dst,
						     rctx->offset);
				rctx->err = spacc_sw_crypt(ctx, s, d,
							   rctx->sw_tail_len,
							   rctx->iv, 0);
			}

			if (!rctx->err)
				memcpy(req->iv, rctx->iv, AES_BLOCK_SIZE);

			spacc_finish_request(req, rctx->err);
		}
	}
}

static bool spacc_submit_next_chunk(struct spacc_hw *hw,
				    struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	bool in_place = (req->src == req->dst);
	unsigned int left = req->cryptlen - rctx->offset;
	unsigned int slot_idx = hw->head;
	struct spacc_hw_slot *slot = &hw->ring[slot_idx];
	unsigned int chunk, src_nents = 0, dst_nents = 0;
	void __iomem *page;
	unsigned long flags;
	unsigned long key_gen;
	u32 ctrl;

	if (unlikely(READ_ONCE(hw->shutting_down) || READ_ONCE(hw->hw_dead)))
		return false;

	chunk = min_t(unsigned int, left, SPACC_MAX_CHUNK_BYTES);

	/* Never let a chunk cross the 16-bit hardware counter boundary. */
	{
		u32 low = ((u32)rctx->iv[14] << 8) | rctx->iv[15];
		u64 limit = (0x10000ULL - low) * AES_BLOCK_SIZE;

		if (limit < chunk)
			chunk = (unsigned int)limit;
	}

	chunk = spacc_build_chunk_sgl(req->src, rctx->offset, chunk,
				      slot->src_sgl, SPACC_MAX_SG_ENTS,
				      &src_nents);
	if (!in_place)
		chunk = spacc_build_chunk_sgl(req->dst, rctx->offset, chunk,
					      slot->dst_sgl, SPACC_MAX_SG_ENTS,
					      &dst_nents);
	else
		dst_nents = src_nents;

	/* Chunk end on a 32-byte boundary keeps cache maintenance in range. */
	chunk = round_down(chunk, 2 * AES_BLOCK_SIZE);
	if (chunk < SPACC_MIN_TAIL_HW_BYTES)
		goto sw_tail;

	spacc_trim_sgl(slot->src_sgl, &src_nents, chunk);
	if (!in_place)
		spacc_trim_sgl(slot->dst_sgl, &dst_nents, chunk);

	/*
	 * On non-coherent ARMv7 (32-byte D-cache line), keep non-final chunk
	 * boundaries aligned to 32 bytes in destination page offsets so a later
	 * chunk's dma_unmap_sg() invalidate never clobbers CPU tail fixup.
	 */
	if (chunk < left) {
		const struct scatterlist *dst_end_sgl =
			in_place ? &slot->src_sgl[src_nents - 1] :
				   &slot->dst_sgl[dst_nents - 1];
		unsigned int end_misalign =
			(dst_end_sgl->offset + dst_end_sgl->length) & 31;

		if (end_misalign) {
			if (end_misalign % AES_BLOCK_SIZE == 0 &&
			    chunk - AES_BLOCK_SIZE >= SPACC_MIN_TAIL_HW_BYTES) {
				chunk -= AES_BLOCK_SIZE;
				spacc_trim_sgl(slot->src_sgl, &src_nents,
					       chunk);
				if (!in_place)
					spacc_trim_sgl(slot->dst_sgl,
						       &dst_nents, chunk);
				dst_end_sgl = in_place ?
					&slot->src_sgl[src_nents - 1] :
					&slot->dst_sgl[dst_nents - 1];
				if ((dst_end_sgl->offset +
				     dst_end_sgl->length) & 31)
					rctx->serialize_chunks = true;
			} else {
				rctx->serialize_chunks = true;
			}
		}
	}

	if (!spacc_map_sg_to_ddt(hw, slot->src_sgl, chunk, src_nents,
				 in_place ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE,
				 slot->src_ddt))
		goto sw_tail;

	if (!in_place &&
	    !spacc_map_sg_to_ddt(hw, slot->dst_sgl, chunk, dst_nents,
				 DMA_FROM_DEVICE, slot->dst_ddt)) {
		dma_unmap_sg(hw->dev, slot->src_sgl, src_nents, DMA_TO_DEVICE);
		goto sw_tail;
	}

	slot->in_place = in_place;
	slot->mapped = true;

	/* Save what the tail fix will need before the engine overwrites it. */
	slot->tail_len = min_t(unsigned int, SPACC_TAIL_FIX_BYTES, chunk);
	scatterwalk_map_and_copy(slot->tail_src, slot->src_sgl,
				 chunk - slot->tail_len, slot->tail_len, 0);
	memcpy(slot->tail_iv, rctx->iv, AES_BLOCK_SIZE);
	spacc_ctr_advance_blocks(slot->tail_iv,
				 (chunk - slot->tail_len) / AES_BLOCK_SIZE);

	slot->req = req;
	slot->src_nents = src_nents;
	slot->dst_nents = dst_nents;
	slot->len = chunk;
	slot->err = 0;

	ctrl = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_CTR |
	       SPA_CTRL_ENCRYPT | (slot_idx << SPA_CTRL_CTX_IDX);

	spin_lock_irqsave(&hw->hw_lock, flags);
	if (hw->shutting_down || hw->resetting || hw->hw_dead) {
		spin_unlock_irqrestore(&hw->hw_lock, flags);
		spacc_unmap_slot(hw, slot);
		spacc_clear_slot_secrets(slot);
		slot->req = NULL;
		return false;
	}

	if (readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) & SPA_FIFO_CMD_FULL) {
		bool need_kick = (hw->hw_inflight == 0);

		spin_unlock_irqrestore(&hw->hw_lock, flags);
		dev_warn_ratelimited(hw->dev,
				     "SPACC: command FIFO full with free ring slot\n");
		spacc_unmap_slot(hw, slot);
		spacc_clear_slot_secrets(slot);
		slot->req = NULL;
		if (need_kick && !READ_ONCE(hw->shutting_down))
			mod_delayed_work(hw->wq, &hw->watchdog,
					 msecs_to_jiffies(100));
		return false;
	}

	/*
	 * Context SRAM is written under hw_lock so a watchdog reset cannot
	 * wipe it between these stores and the CTRL write.
	 */
	page = hw->cipher_ctx_base + (slot_idx * SPACC_CIPHER_PG_SZ);
	key_gen = smp_load_acquire(&ctx->key_gen);
	if (slot->last_ctx != ctx ||
	    slot->last_key_gen != key_gen ||
	    slot->last_keylen != ctx->keylen) {
		spacc_memcpy_toio32(page, ctx->key, ctx->keylen);
		slot->last_ctx = ctx;
		slot->last_key_gen = key_gen;
		slot->last_keylen = ctx->keylen;
	}
	spacc_memcpy_toio32(page + SPACC_CIPHER_IV_OFFSET, rctx->iv,
			    AES_BLOCK_SIZE);
	(void)readl(page);

	spacc_ctr_advance_blocks(rctx->iv, chunk / AES_BLOCK_SIZE);

	writel_relaxed(ctx->keylen | (slot_idx << SPA_KEY_SZ_CTX_INDEX_OFFSET) |
		       SPA_KEY_SZ_CIPHER,
		       hw->crypto_regs + SPACC_REG_KEY_SZ);
	writel_relaxed(lower_32_bits(slot->src_ddt_dma),
		       hw->crypto_regs + SPACC_REG_SRC_PTR);
	writel_relaxed(lower_32_bits(in_place ? slot->src_ddt_dma :
						slot->dst_ddt_dma),
		       hw->crypto_regs + SPACC_REG_DST_PTR);
	writel_relaxed(chunk, hw->crypto_regs + SPACC_REG_PROC_LEN);
	writel_relaxed(slot_idx, hw->crypto_regs + SPACC_REG_SW_ID);
	writel(ctrl, hw->crypto_regs + SPACC_REG_CTRL);

	hw->head = (hw->head + 1) % SPACC_RING_SIZE;
	if (hw->hw_inflight == 0) {
		hw->last_progress = jiffies;
		if (!hw->watchdog_armed && !READ_ONCE(hw->shutting_down)) {
			hw->watchdog_armed = true;
			mod_delayed_work(hw->wq, &hw->watchdog,
					 msecs_to_jiffies(1000));
		}
	}
	hw->hw_inflight++;
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	rctx->offset += chunk;
	rctx->inflight_cmds++;

	return true;

sw_tail:
	rctx->sw_tail_len = left;
	if (rctx->inflight_cmds == 0) {
		struct scatterlist sc[2], dc[2];
		struct scatterlist *s, *d;
		int err;

		s = scatterwalk_ffwd(sc, req->src, rctx->offset);
		d = in_place ? s : scatterwalk_ffwd(dc, req->dst, rctx->offset);
		err = spacc_sw_crypt(ctx, s, d, left, rctx->iv, 0);

		if (!err)
			memcpy(req->iv, rctx->iv, AES_BLOCK_SIZE);
		spacc_finish_request(req, err);
	}
	return true;
}

static void spacc_drain_dead_hw(struct spacc_hw *hw)
{
	struct spacc_reqctx *rctx, *tmp;
	struct crypto_async_request *areq;

	list_for_each_entry_safe(rctx, tmp, &hw->active_reqs, node) {
		struct skcipher_request *req = rctx->req;
		struct spacc_ctx *ctx =
			crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
		unsigned int left = req->cryptlen - rctx->offset;

		if (rctx->inflight_cmds != 0)
			continue;

		if (!rctx->err && left > 0) {
			struct scatterlist sc[2], dc[2];
			struct scatterlist *s, *d;

			s = scatterwalk_ffwd(sc, req->src, rctx->offset);
			d = (req->src == req->dst) ? s :
			    scatterwalk_ffwd(dc, req->dst, rctx->offset);
			rctx->err = spacc_sw_crypt(ctx, s, d, left, rctx->iv, 0);
		}
		if (!rctx->err)
			memcpy(req->iv, rctx->iv, AES_BLOCK_SIZE);
		spacc_finish_request(req, rctx->err);
	}

	for (;;) {
		struct crypto_async_request *backlog;
		struct skcipher_request *req;
		struct spacc_ctx *ctx;
		int err;

		spin_lock_bh(&hw->q_lock);
		backlog = crypto_get_backlog(&hw->queue);
		areq = crypto_dequeue_request(&hw->queue);
		spin_unlock_bh(&hw->q_lock);

		if (!areq)
			break;

		if (backlog) {
			local_bh_disable();
			backlog->complete(backlog, -EINPROGRESS);
			local_bh_enable();
		}

		req = skcipher_request_cast(areq);
		ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
		err = spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				     req->iv, 0);
		spacc_finish_request(req, err);
	}
}

static void spacc_work_fn(struct work_struct *work)
{
	struct spacc_hw *hw = container_of(work, struct spacc_hw, work);

	if (unlikely(READ_ONCE(hw->shutting_down)))
		return;

	if (unlikely(READ_ONCE(hw->hw_dead))) {
		spacc_drain_dead_hw(hw);
		return;
	}

	spacc_reap_completed(hw);

	while (((hw->head + 1) % SPACC_RING_SIZE) != hw->tail) {
		struct skcipher_request *target = NULL;
		struct spacc_reqctx *rctx;

		if (unlikely(READ_ONCE(hw->shutting_down)))
			return;

		if (unlikely(READ_ONCE(hw->hw_dead))) {
			spacc_drain_dead_hw(hw);
			return;
		}

		list_for_each_entry(rctx, &hw->active_reqs, node) {
			struct skcipher_request *cand = rctx->req;

			if (rctx->err || rctx->sw_tail_len ||
			    rctx->offset >= cand->cryptlen)
				continue;
			if (rctx->serialize_chunks && rctx->inflight_cmds > 0)
				continue;

			target = cand;
			break;
		}

		if (!target) {
			struct crypto_async_request *areq, *backlog;

			spin_lock_bh(&hw->q_lock);
			backlog = crypto_get_backlog(&hw->queue);
			areq = crypto_dequeue_request(&hw->queue);
			spin_unlock_bh(&hw->q_lock);

			if (!areq)
				break;

			if (backlog) {
				local_bh_disable();
				backlog->complete(backlog, -EINPROGRESS);
				local_bh_enable();
			}

			target = skcipher_request_cast(areq);
			rctx = skcipher_request_ctx(target);
			list_add_tail(&rctx->node, &hw->active_reqs);
		}

		if (!spacc_submit_next_chunk(hw, target))
			break;
	}

	if (!unlikely(READ_ONCE(hw->shutting_down)))
		spacc_reap_completed(hw);
}

static void spacc_watchdog_fn(struct work_struct *work)
{
	struct spacc_hw *hw = container_of(work, struct spacc_hw,
					    watchdog.work);
	unsigned long flags;
	int ret;

	if (READ_ONCE(hw->shutting_down))
		return;

	spin_lock_irqsave(&hw->hw_lock, flags);
	if (hw->hw_inflight == 0) {
		hw->watchdog_armed = false;
		spin_unlock_irqrestore(&hw->hw_lock, flags);
		return;
	}

	if (!time_after(jiffies,
			hw->last_progress + msecs_to_jiffies(1000))) {
		mod_delayed_work(hw->wq, &hw->watchdog,
				 msecs_to_jiffies(1000));
		spin_unlock_irqrestore(&hw->hw_lock, flags);
		return;
	}

	hw->resetting = true;
	writel(0, hw->pdu_regs + SPACC_REG_IRQ_EN);
	(void)readl(hw->pdu_regs + SPACC_REG_IRQ_EN);
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	dev_err_ratelimited(hw->dev,
			    "SPACC: command stalled (IRQ_STAT=0x%08x FIFO_STAT=0x%08x), resetting\n",
			    readl(hw->crypto_regs + SPACC_REG_IRQ_STAT),
			    readl(hw->crypto_regs + SPACC_REG_FIFO_STAT));

	synchronize_irq(hw->irq);

	ret = reset_control_assert(hw->rst);
	if (!ret) {
		fsleep(10);
		ret = reset_control_deassert(hw->rst);
	}
	if (ret) {
		dev_err(hw->dev,
			"SPACC: hardware reset failed (%d), disabling DMA offload\n",
			ret);
		spin_lock_irqsave(&hw->hw_lock, flags);
		WRITE_ONCE(hw->hw_dead, true);
		hw->watchdog_armed = false;
		hw->resetting = false;
		spin_unlock_irqrestore(&hw->hw_lock, flags);

		if (!READ_ONCE(hw->shutting_down))
			queue_work(hw->wq, &hw->work);
		return;
	}

	spin_lock_irqsave(&hw->hw_lock, flags);
	spacc_hw_init_regs(hw, true);
	spacc_invalidate_all_contexts(hw);
	while (hw->hw_inflight > 0) {
		struct spacc_hw_slot *slot = &hw->ring[hw->complete_idx];

		slot->err = -ETIMEDOUT;
		hw->complete_idx = (hw->complete_idx + 1) % SPACC_RING_SIZE;
		hw->hw_inflight--;
	}
	hw->watchdog_armed = false;
	hw->resetting = false;
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	if (!READ_ONCE(hw->shutting_down))
		queue_work(hw->wq, &hw->work);
}

static int spacc_cra_init(struct crypto_skcipher *tfm)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct spacc_alg *salg = container_of(alg, struct spacc_alg, alg);
	struct spacc_dev *sdev = salg->sdev;
	const char *name = crypto_tfm_alg_name(&tfm->base);

	if (!kref_get_unless_zero(&sdev->refcount))
		return -ENODEV;

	ctx->sdev = sdev;
	mutex_init(&ctx->lock);

	ctx->fallback_tfm = crypto_alloc_sync_skcipher(name, 0,
						       CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->fallback_tfm)) {
		int err = PTR_ERR(ctx->fallback_tfm);

		ctx->fallback_tfm = NULL;
		mutex_destroy(&ctx->lock);
		kref_put(&sdev->refcount, spacc_dev_release);
		return err;
	}

	crypto_skcipher_set_reqsize(tfm, sizeof(struct spacc_reqctx));
	return 0;
}

static void spacc_cra_exit(struct crypto_skcipher *tfm)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);

	if (ctx->fallback_tfm) {
		crypto_free_sync_skcipher(ctx->fallback_tfm);
		ctx->fallback_tfm = NULL;
	}
	memzero_explicit(&ctx->aes_ctx, sizeof(ctx->aes_ctx));
	memzero_explicit(ctx->key, sizeof(ctx->key));
	mutex_destroy(&ctx->lock);
	kref_put(&ctx->sdev->refcount, spacc_dev_release);
}

static int spacc_aes_setkey(struct crypto_skcipher *tfm, const u8 *key,
			    unsigned int keylen)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	int err;

	if (keylen != AES_KEYSIZE_128 && keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_256)
		return -EINVAL;

	mutex_lock(&ctx->lock);

	crypto_sync_skcipher_clear_flags(ctx->fallback_tfm, CRYPTO_TFM_REQ_MASK);
	crypto_sync_skcipher_set_flags(ctx->fallback_tfm,
				       crypto_skcipher_get_flags(tfm) &
				       CRYPTO_TFM_REQ_MASK);

	err = crypto_sync_skcipher_setkey(ctx->fallback_tfm, key, keylen);
	if (!err)
		err = aes_expandkey(&ctx->aes_ctx, key, keylen);

	if (!err) {
		memzero_explicit(ctx->key, sizeof(ctx->key));
		memcpy(ctx->key, key, keylen);
		ctx->keylen = keylen;
		smp_store_release(&ctx->key_gen,
				  (unsigned long)atomic_long_inc_return(&spacc_key_gen_seq));
	} else {
		memzero_explicit(&ctx->aes_ctx, sizeof(ctx->aes_ctx));
	}

	mutex_unlock(&ctx->lock);
	return err;
}

static int spacc_aes_crypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	struct spacc_hw *hw;
	int err;

	if (req->cryptlen == 0)
		return 0;

	if (req->cryptlen < SPACC_MIN_HW_BYTES)
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, req->base.flags);

	rcu_read_lock();
	hw = rcu_dereference(ctx->sdev->hw);
	if (unlikely(!hw || READ_ONCE(hw->shutting_down) ||
		     READ_ONCE(hw->hw_dead))) {
		rcu_read_unlock();
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, req->base.flags);
	}

	INIT_LIST_HEAD(&rctx->node);
	rctx->req = req;
	rctx->offset = 0;
	rctx->inflight_cmds = 0;
	rctx->sw_tail_len = 0;
	rctx->serialize_chunks = false;
	rctx->err = 0;
	memcpy(rctx->iv, req->iv, AES_BLOCK_SIZE);

	spin_lock_bh(&hw->q_lock);
	if (hw->queue.qlen >= hw->queue.max_qlen &&
	    !(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG)) {
		spin_unlock_bh(&hw->q_lock);
		rcu_read_unlock();
		memzero_explicit(rctx->iv, sizeof(rctx->iv));
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, req->base.flags);
	}
	err = crypto_enqueue_request(&hw->queue, &req->base);
	spin_unlock_bh(&hw->q_lock);

	if (err == -ENOSPC) {
		rcu_read_unlock();
		memzero_explicit(rctx->iv, sizeof(rctx->iv));
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, req->base.flags);
	}

	queue_work(hw->wq, &hw->work);
	rcu_read_unlock();

	return err;
}

static const struct spacc_alg spacc_alg_tmpl = {
	.alg = {
		.base.cra_name		= "ctr(aes)",
		.base.cra_driver_name	= "ctr-aes-ls1024a-spacc",
		.base.cra_priority	= 500,
		.base.cra_flags		= CRYPTO_ALG_ASYNC |
					  CRYPTO_ALG_NEED_FALLBACK,
		.base.cra_blocksize	= 1,
		.base.cra_ctxsize	= sizeof(struct spacc_ctx),
		.base.cra_alignmask	= 0,
		.base.cra_module	= THIS_MODULE,

		.init			= spacc_cra_init,
		.exit			= spacc_cra_exit,
		.min_keysize		= AES_MIN_KEY_SIZE,
		.max_keysize		= AES_MAX_KEY_SIZE,
		.ivsize			= AES_BLOCK_SIZE,
		.setkey			= spacc_aes_setkey,
		.encrypt		= spacc_aes_crypt,
		.decrypt		= spacc_aes_crypt,
	},
};

static int spacc_probe(struct platform_device *pdev)
{
	struct spacc_dev *sdev;
	struct spacc_hw *hw;
	struct resource *res;
	unsigned long flags;
	size_t ddt_stride, ddt_bytes;
	u32 spacc_id;
	int ret, i;

	sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	kref_init(&sdev->refcount);

	hw = devm_kzalloc(&pdev->dev, sizeof(*hw), GFP_KERNEL);
	if (!hw) {
		ret = -ENOMEM;
		goto err_sdev_put;
	}

	hw->dev = &pdev->dev;
	spin_lock_init(&hw->q_lock);
	spin_lock_init(&hw->hw_lock);
	crypto_init_queue(&hw->queue, SPACC_QUEUE_LEN);
	INIT_LIST_HEAD(&hw->active_reqs);
	INIT_WORK(&hw->work, spacc_work_fn);
	INIT_DELAYED_WORK(&hw->watchdog, spacc_watchdog_fn);

	hw->clk = devm_clk_get_optional(&pdev->dev, "spacc");
	if (IS_ERR(hw->clk)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(hw->clk),
				    "Failed to get spacc clock\n");
		goto err_sdev_put;
	}

	if (hw->clk) {
		ret = clk_prepare_enable(hw->clk);
		if (ret) {
			dev_err_probe(&pdev->dev, ret,
				      "Failed to enable spacc clock\n");
			goto err_sdev_put;
		}
	}

	hw->rst = devm_reset_control_get_exclusive(&pdev->dev, "spacc");
	if (IS_ERR(hw->rst)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(hw->rst),
				    "Failed to get exclusive spacc reset\n");
		goto err_clk_disable;
	}

	ret = reset_control_deassert(hw->rst);
	if (ret) {
		dev_err(&pdev->dev, "Failed to deassert reset: %d\n", ret);
		goto err_clk_disable;
	}

	hw->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hw->regs)) {
		ret = PTR_ERR(hw->regs);
		goto err_reset_assert;
	}

	resource_size_t res_size = resource_size(res);

	if (res_size < SPACC_MIN_MMIO_SIZE) {
		dev_err(&pdev->dev, "MMIO region too small (%pa < %#x)\n",
			&res_size, SPACC_MIN_MMIO_SIZE);
		ret = -EINVAL;
		goto err_reset_assert;
	}

	hw->pdu_regs = hw->regs;
	hw->crypto_regs = hw->regs + SPACC_CRYPTO_BASE_OFFSET;
	hw->cipher_ctx_base = hw->crypto_regs + SPACC_REG_CIPH_KEY_BASE;

	spacc_id = readl(hw->pdu_regs + SPACC_REG_ID);
	dev_info(&pdev->dev,
		 "LS1024A SPACC CTR engine probed (version %u.%u)\n",
		 (spacc_id >> 4) & 0x0f, spacc_id & 0x0f);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
		goto err_reset_assert;
	}

	dma_set_max_seg_size(&pdev->dev, SPACC_MAX_CHUNK_BYTES);
	dma_set_seg_boundary(&pdev->dev, DMA_BIT_MASK(16));

	ddt_stride = SPACC_MAX_DDT_ENTS * sizeof(struct spacc_ddt);
	ddt_bytes = SPACC_RING_SIZE * 2 * ddt_stride;
	hw->ddt_pool = dmam_alloc_coherent(&pdev->dev, ddt_bytes,
					   &hw->ddt_pool_dma, GFP_KERNEL);
	if (!hw->ddt_pool) {
		ret = -ENOMEM;
		goto err_reset_assert;
	}

	for (i = 0; i < SPACC_RING_SIZE; i++) {
		struct spacc_hw_slot *slot = &hw->ring[i];
		unsigned int base_idx = i * 2;

		slot->src_ddt = hw->ddt_pool + base_idx * SPACC_MAX_DDT_ENTS;
		slot->src_ddt_dma = hw->ddt_pool_dma + base_idx * ddt_stride;
		slot->dst_ddt = hw->ddt_pool +
				(base_idx + 1) * SPACC_MAX_DDT_ENTS;
		slot->dst_ddt_dma = hw->ddt_pool_dma +
				    (base_idx + 1) * ddt_stride;
	}

	/*
	 * One worker owns the slot ring and the hardware contexts, so max_active
	 * is 1 by design: pipeline depth comes from the ring.
	 */
	hw->wq = alloc_workqueue("ls1024a-spacc",
				 WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!hw->wq) {
		ret = -ENOMEM;
		goto err_reset_assert;
	}

	hw->irq = platform_get_irq(pdev, 0);
	if (hw->irq < 0) {
		ret = hw->irq;
		goto err_wq_destroy;
	}

	/* Initialize registers with IRQs masked until handler is installed. */
	spin_lock_irqsave(&hw->hw_lock, flags);
	spacc_hw_init_regs(hw, false);
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	ret = devm_request_irq(&pdev->dev, hw->irq, spacc_irq_handler, 0,
			       dev_name(&pdev->dev), hw);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n",
			hw->irq, ret);
		goto err_wq_destroy;
	}

	spin_lock_irqsave(&hw->hw_lock, flags);
	spacc_enable_irqs(hw);
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	sdev->alg = spacc_alg_tmpl;
	sdev->alg.sdev = sdev;
	rcu_assign_pointer(sdev->hw, hw);
	platform_set_drvdata(pdev, sdev);

	ret = crypto_register_skcipher(&sdev->alg.alg);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register %s: %d\n",
			sdev->alg.alg.base.cra_name, ret);
		goto err_unpublish;
	}

	return 0;

err_unpublish:
	WRITE_ONCE(hw->shutting_down, true);
	RCU_INIT_POINTER(sdev->hw, NULL);
	synchronize_rcu();
	writel(0, hw->pdu_regs + SPACC_REG_IRQ_EN);
	(void)readl(hw->pdu_regs + SPACC_REG_IRQ_EN);
	devm_free_irq(&pdev->dev, hw->irq, hw);
err_wq_destroy:
	destroy_workqueue(hw->wq);
err_reset_assert:
	reset_control_assert(hw->rst);
err_clk_disable:
	if (hw->clk)
		clk_disable_unprepare(hw->clk);
err_sdev_put:
	kref_put(&sdev->refcount, spacc_dev_release);
	return ret;
}

static void spacc_remove(struct platform_device *pdev)
{
	struct spacc_dev *sdev = platform_get_drvdata(pdev);
	struct spacc_hw *hw = rcu_dereference_protected(sdev->hw, 1);
	struct spacc_reqctx *rctx, *tmp;
	struct crypto_async_request *areq;
	unsigned long flags;
	int i;

	crypto_unregister_skcipher(&sdev->alg.alg);

	/*
	 * Unpublish hardware and wait for all RCU readers (spacc_aes_crypt)
	 * to finish before stopping workers or freeing hardware resources.
	 * Surviving TFMs will take the software fallback path cleanly.
	 */
	WRITE_ONCE(hw->shutting_down, true);
	RCU_INIT_POINTER(sdev->hw, NULL);
	synchronize_rcu();

	writel(0, hw->pdu_regs + SPACC_REG_IRQ_EN);
	(void)readl(hw->pdu_regs + SPACC_REG_IRQ_EN);
	synchronize_irq(hw->irq);

	/*
	 * Timer first: a running watchdog queues the worker on its way out, so
	 * cancelling the worker before the timer would leave that work pending.
	 */
	cancel_delayed_work_sync(&hw->watchdog);
	cancel_work_sync(&hw->work);

	/*
	 * Unconditionally pulse reset to stop any in-flight or posted DMA
	 * writes before unmapping buffers; re-enable so context SRAM can be
	 * wiped below.
	 */
	reset_control_assert(hw->rst);
	udelay(20);
	reset_control_deassert(hw->rst);
	udelay(20);

	spin_lock_irqsave(&hw->hw_lock, flags);
	for (i = 0; i < SPACC_RING_SIZE; i++)
		hw->ring[i].req = NULL;
	hw->head = 0;
	hw->tail = 0;
	hw->complete_idx = 0;
	hw->hw_inflight = 0;
	hw->watchdog_armed = false;
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	for (i = 0; i < SPACC_RING_SIZE; i++) {
		spacc_unmap_slot(hw, &hw->ring[i]);
		spacc_clear_slot_secrets(&hw->ring[i]);
	}

	list_for_each_entry_safe(rctx, tmp, &hw->active_reqs, node)
		spacc_finish_request(rctx->req, -EIO);

	for (;;) {
		spin_lock_bh(&hw->q_lock);
		areq = crypto_dequeue_request(&hw->queue);
		spin_unlock_bh(&hw->q_lock);

		if (!areq)
			break;
		spacc_finish_request(skcipher_request_cast(areq), -EIO);
	}

	devm_free_irq(&pdev->dev, hw->irq, hw);
	destroy_workqueue(hw->wq);

	/* Key material survives cra_exit in the MMIO context pages: wipe. */
	for (i = 0; i < SPACC_RING_SIZE; i++)
		memset_io(hw->cipher_ctx_base + (i * SPACC_CIPHER_PG_SZ),
			  0, SPACC_CIPHER_PG_SZ);

	reset_control_assert(hw->rst);

	if (hw->clk)
		clk_disable_unprepare(hw->clk);

	kref_put(&sdev->refcount, spacc_dev_release);
}

static const struct of_device_id spacc_of_match[] = {
	{ .compatible = "fsl,ls1024a-spacc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spacc_of_match);

static struct platform_driver spacc_driver = {
	.probe		= spacc_probe,
	.remove		= spacc_remove,
	.driver		= {
		.name	= DRV_NAME,
		.of_match_table = spacc_of_match,
	},
};
module_platform_driver(spacc_driver);

MODULE_DESCRIPTION("LS1024A SPACC AES-CTR driver");
MODULE_AUTHOR("mrrdmr");
MODULE_LICENSE("GPL");
