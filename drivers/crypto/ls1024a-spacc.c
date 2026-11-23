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
 * Copyright (C) 2026 Antigravity Project
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
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
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
 * spacc_sg_span().
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

struct spacc_ddt {
	u32 p;
	u32 len;
};

struct spacc_ctx;
struct spacc_dev;

struct spacc_alg {
	struct skcipher_alg alg;
	struct spacc_dev *spacc;
};

struct spacc_hw_slot {
	struct skcipher_request *req;
	/*
	 * Private bounded chains: every in-flight chunk owns its sg array, so a
	 * dma_map_sg() of one chunk can never alias another chunk of the same
	 * request.
	 */
	struct scatterlist src_chain[2];
	struct scatterlist dst_chain[2];
	struct scatterlist *src;
	struct scatterlist *dst;
	struct spacc_ddt *src_ddt;
	struct spacc_ddt *dst_ddt;
	dma_addr_t src_ddt_dma;
	dma_addr_t dst_ddt_dma;
	unsigned int src_nents;
	unsigned int dst_nents;
	unsigned int len;
	unsigned int tail_len;
	bool mapped;
	int err;

	/* Context SRAM key cache metadata */
	const struct spacc_ctx *last_ctx;
	u32 last_key_gen;
	unsigned int last_keylen;

	u8 tail_src[SPACC_TAIL_FIX_BYTES] __aligned(4);
	u8 tail_iv[AES_BLOCK_SIZE] __aligned(4);
};

struct spacc_dev {
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

	struct workqueue_struct *wq;
	struct work_struct work;
	struct delayed_work watchdog;
	bool shutting_down;

	struct spacc_alg alg;
};

struct spacc_ctx {
	struct spacc_dev *spacc;
	struct crypto_sync_skcipher *fallback_tfm;
	u8 key[AES_MAX_KEY_SIZE] __aligned(4);
	unsigned int keylen;
	u32 key_gen;
	struct mutex lock;
};

struct spacc_reqctx {
	struct list_head node;
	struct skcipher_request *req;
	unsigned int offset;
	unsigned int inflight_cmds;
	unsigned int sw_tail_len;
	int err;
	u8 iv[AES_BLOCK_SIZE] __aligned(4);
};

static atomic_t spacc_key_gen_seq = ATOMIC_INIT(0);

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
		writel_relaxed(get_unaligned((const u32 *)(src8 + i * 4)),
			       dst32 + i);
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

static unsigned int spacc_sg_span(struct scatterlist *sgl,
				  unsigned int max_bytes,
				  unsigned int max_ents,
				  unsigned int *nents)
{
	struct scatterlist *sg = sgl;
	unsigned int total = 0, used = 0;

	while (sg && used < max_ents && total < max_bytes) {
		unsigned int take = min_t(unsigned int, sg->length,
					  max_bytes - total);

		total += take;
		used++;
		sg = sg_next(sg);
	}

	*nents = used;
	return total;
}

static bool spacc_map_sg_to_ddt(struct spacc_dev *spacc,
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

	mapped_ents = dma_map_sg(spacc->dev, sgl, nents, dir);
	if (mapped_ents <= 0)
		return false;

	for_each_sg(sgl, cur, mapped_ents, i) {
		unsigned int len = min_t(unsigned int, sg_dma_len(cur), nbytes);

		if (!len)
			continue;
		ddt[ddt_idx].p = lower_32_bits(sg_dma_address(cur));
		ddt[ddt_idx].len = len;
		ddt_idx++;
		nbytes -= len;
		if (nbytes == 0)
			break;
	}
	ddt[ddt_idx].p = 0;
	ddt[ddt_idx].len = 0;

	/* Read back the terminator: drains the write path before CTRL starts. */
	(void)READ_ONCE(ddt[ddt_idx].len);
	dma_wmb();

	return true;
}

/* Full 128-bit big-endian counter increment, matching NIST SP 800-38A */
static void spacc_ctr_advance(u8 *iv, unsigned int bytes)
{
	u64 add = bytes / AES_BLOCK_SIZE;
	u32 carry = 0;
	int i;

	for (i = 15; i >= 0 && (add || carry); i--) {
		u32 v = iv[i] + (u32)(add & 0xff) + carry;

		iv[i] = v & 0xff;
		carry = v >> 8;
		add >>= 8;
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

static void spacc_hw_init_regs(struct spacc_dev *spacc)
{
	int limit = 64;

	lockdep_assert_held(&spacc->hw_lock);

	while (limit-- &&
	       !(readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		writel(~0U, spacc->crypto_regs + SPACC_REG_STAT_POP);
		(void)readl(spacc->crypto_regs + SPACC_REG_STATUS);
	}

	/* Constant zero fields in pure AES-CTR mode: program once on init */
	writel_relaxed(0, spacc->crypto_regs + SPACC_REG_OFFSET);
	writel_relaxed(0, spacc->crypto_regs + SPACC_REG_PRE_AAD_LEN);
	writel_relaxed(0, spacc->crypto_regs + SPACC_REG_POST_AAD_LEN);
	writel_relaxed(0, spacc->crypto_regs + SPACC_REG_ICV_LEN);
	writel_relaxed(0, spacc->crypto_regs + SPACC_REG_ICV_OFFSET);
	writel_relaxed(0, spacc->crypto_regs + SPACC_REG_IV_OFFSET);
	writel_relaxed(0, spacc->crypto_regs + SPACC_REG_AUX_INFO);

	writel(~0U, spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(~0U, spacc->pdu_regs + SPACC_REG_IRQ_STAT);

	/* STAT_CNT fixed at 1: one status entry per interrupt */
	writel(1U << SPA_IRQ_CTRL_STAT_CNT_OFFSET,
	       spacc->crypto_regs + SPACC_REG_IRQ_CTRL);
	writel(SPA_IRQ_EN_STAT_EN | SPA_IRQ_EN_GLBL_EN,
	       spacc->pdu_regs + SPACC_REG_IRQ_EN);
}

static irqreturn_t spacc_irq_handler(int irq, void *dev_id)
{
	struct spacc_dev *spacc = dev_id;
	unsigned long flags;
	u32 stat, cstat, status, code;
	bool completed_any = false;

	spin_lock_irqsave(&spacc->hw_lock, flags);

	stat = readl(spacc->pdu_regs + SPACC_REG_IRQ_STAT);
	if (!(stat & (SPA_IRQ_STAT_STAT_MASK | SPA_IRQ_STAT_CMD))) {
		spin_unlock_irqrestore(&spacc->hw_lock, flags);
		return IRQ_NONE;
	}

	cstat = readl(spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	if (cstat)
		writel(cstat, spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(stat, spacc->pdu_regs + SPACC_REG_IRQ_STAT);

	while (spacc->hw_inflight > 0 &&
	       !(readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		struct spacc_hw_slot *slot = &spacc->ring[spacc->complete_idx];

		writel(~0U, spacc->crypto_regs + SPACC_REG_STAT_POP);
		status = readl(spacc->crypto_regs + SPACC_REG_STATUS);
		code = (status & SPA_STATUS_RES_CODE_MASK) >>
		       SPA_STATUS_RES_CODE_OFFSET;
		if (code != SPA_STATUS_OK)
			dev_warn_ratelimited(spacc->dev,
					     "SPACC: command failed, STATUS=0x%08x (code %u)\n",
					     status, code);

		slot->err = spacc_status_to_errno(code);
		spacc->complete_idx = (spacc->complete_idx + 1) % SPACC_RING_SIZE;
		spacc->hw_inflight--;
		spacc->last_progress = jiffies;
		completed_any = true;
	}

	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	if (completed_any)
		queue_work(spacc->wq, &spacc->work);

	return IRQ_HANDLED;
}

static void spacc_unmap_slot(struct spacc_dev *spacc,
			   struct spacc_hw_slot *slot)
{
	if (!slot->mapped)
		return;

	if (slot->src == slot->dst) {
		dma_unmap_sg(spacc->dev, slot->src, slot->src_nents,
			     DMA_BIDIRECTIONAL);
	} else {
		dma_unmap_sg(spacc->dev, slot->src, slot->src_nents,
			     DMA_TO_DEVICE);
		dma_unmap_sg(spacc->dev, slot->dst, slot->dst_nents,
			     DMA_FROM_DEVICE);
	}
	slot->mapped = false;
}

/*
 * Recompute the command tail on the CPU.  The engine's last write burst may
 * have lost or scrambled 8-24 bytes; the source bytes were saved before the
 * command, so this works for in-place buffers too.
 */
static int spacc_fix_tail(struct spacc_ctx *ctx, struct spacc_hw_slot *slot)
{
	struct scatterlist src_sg, dc[2];
	struct scatterlist *dst;

	if (!slot->tail_len)
		return 0;

	sg_init_table(&src_sg, 1);
	sg_set_buf(&src_sg, slot->tail_src, slot->tail_len);
	dst = scatterwalk_ffwd(dc, slot->dst, slot->len - slot->tail_len);

	return spacc_sw_crypt(ctx, &src_sg, dst, slot->tail_len, slot->tail_iv, 0);
}

static void spacc_finish_request(struct skcipher_request *req, int err)
{
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);

	list_del_init(&rctx->node);
	memzero_explicit(rctx->iv, sizeof(rctx->iv));
	local_bh_disable();
	skcipher_request_complete(req, err);
	local_bh_enable();
}

static void spacc_reap_completed(struct spacc_dev *spacc)
{
	unsigned long flags;
	unsigned int comp_idx;

	spin_lock_irqsave(&spacc->hw_lock, flags);
	comp_idx = spacc->complete_idx;
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	while (spacc->tail != comp_idx) {
		struct spacc_hw_slot *slot = &spacc->ring[spacc->tail];
		struct skcipher_request *req = slot->req;
		struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
		struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
		struct spacc_reqctx *rctx = skcipher_request_ctx(req);
		int slot_err = slot->err;

		spacc_unmap_slot(spacc, slot);

		if (slot_err && !rctx->err)
			rctx->err = slot_err;

		if (!rctx->err)
			rctx->err = spacc_fix_tail(ctx, slot);

		rctx->inflight_cmds--;
		slot->req = NULL;
		spacc->tail = (spacc->tail + 1) % SPACC_RING_SIZE;

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

static bool spacc_submit_next_chunk(struct spacc_dev *spacc,
				    struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	bool in_place = (req->src == req->dst);
	unsigned int left = req->cryptlen - rctx->offset;
	unsigned int slot_idx = spacc->head;
	struct spacc_hw_slot *slot = &spacc->ring[slot_idx];
	unsigned int chunk, src_nents = 0, dst_nents = 0;
	void __iomem *page;
	unsigned long flags;
	u32 key_gen, ctrl;

	slot->src = scatterwalk_ffwd(slot->src_chain, req->src, rctx->offset);
	slot->dst = in_place ? slot->src :
		    scatterwalk_ffwd(slot->dst_chain, req->dst, rctx->offset);

	chunk = min_t(unsigned int, left, SPACC_MAX_CHUNK_BYTES);

	/* Never let a chunk cross the 16-bit hardware counter boundary. */
	{
		u32 low = ((u32)rctx->iv[14] << 8) | rctx->iv[15];
		u64 limit = (0x10000ULL - low) * AES_BLOCK_SIZE;

		if (limit < chunk)
			chunk = (unsigned int)limit;
	}

	chunk = spacc_sg_span(slot->src, chunk, SPACC_MAX_SG_ENTS, &src_nents);
	if (!in_place)
		chunk = spacc_sg_span(slot->dst, chunk, SPACC_MAX_SG_ENTS,
				      &dst_nents);
	else
		dst_nents = src_nents;

	/* Chunk end on a 32-byte boundary keeps cache maintenance in range. */
	chunk = round_down(chunk, 2 * AES_BLOCK_SIZE);

	if (chunk < SPACC_MIN_TAIL_HW_BYTES)
		goto sw_tail;

	if (!spacc_map_sg_to_ddt(spacc, slot->src, chunk, src_nents,
				 in_place ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE,
				 slot->src_ddt))
		goto sw_tail;

	if (!in_place &&
	    !spacc_map_sg_to_ddt(spacc, slot->dst, chunk, dst_nents,
				 DMA_FROM_DEVICE, slot->dst_ddt)) {
		dma_unmap_sg(spacc->dev, slot->src, src_nents, DMA_TO_DEVICE);
		goto sw_tail;
	}

	slot->mapped = true;

	/* Save what the tail fix will need before the engine overwrites it. */
	slot->tail_len = min_t(unsigned int, SPACC_TAIL_FIX_BYTES, chunk);
	scatterwalk_map_and_copy(slot->tail_src, slot->src,
				 chunk - slot->tail_len, slot->tail_len, 0);
	memcpy(slot->tail_iv, rctx->iv, AES_BLOCK_SIZE);
	spacc_ctr_advance(slot->tail_iv, chunk - slot->tail_len);

	slot->req = req;
	slot->src_nents = src_nents;
	slot->dst_nents = dst_nents;
	slot->len = chunk;
	slot->err = 0;

	ctrl = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_CTR |
	       SPA_CTRL_ENCRYPT | (slot_idx << SPA_CTRL_CTX_IDX);

	spin_lock_irqsave(&spacc->hw_lock, flags);
	if (spacc->resetting ||
	    (readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) &
	     SPA_FIFO_CMD_FULL)) {
		spin_unlock_irqrestore(&spacc->hw_lock, flags);
		spacc_unmap_slot(spacc, slot);
		slot->req = NULL;
		return false;
	}

	/*
	 * Context SRAM is written under the lock that excludes the watchdog
	 * reset: a reset wipes it, and must not land between these stores and
	 * the CTRL write.  The key cache is only a hint and is invalidated
	 * whenever the engine is reset.
	 *
	 * setkey() must not race with a request on the same tfm -- that is an
	 * API guarantee -- and the generation counter keeps a stale cache
	 * impossible after rekeying.
	 */
	page = spacc->cipher_ctx_base + (slot_idx * SPACC_CIPHER_PG_SZ);
	key_gen = READ_ONCE(ctx->key_gen);
	smp_rmb();
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

	spacc_ctr_advance(rctx->iv, chunk);

	writel_relaxed(ctx->keylen | (slot_idx << SPA_KEY_SZ_CTX_INDEX_OFFSET) |
		       SPA_KEY_SZ_CIPHER,
		       spacc->crypto_regs + SPACC_REG_KEY_SZ);
	writel_relaxed(lower_32_bits(slot->src_ddt_dma),
		       spacc->crypto_regs + SPACC_REG_SRC_PTR);
	writel_relaxed(lower_32_bits(in_place ? slot->src_ddt_dma :
						slot->dst_ddt_dma),
		       spacc->crypto_regs + SPACC_REG_DST_PTR);
	writel_relaxed(chunk, spacc->crypto_regs + SPACC_REG_PROC_LEN);
	writel_relaxed(slot_idx, spacc->crypto_regs + SPACC_REG_SW_ID);
	writel(ctrl, spacc->crypto_regs + SPACC_REG_CTRL);

	spacc->head = (spacc->head + 1) % SPACC_RING_SIZE;
	if (spacc->hw_inflight == 0) {
		spacc->last_progress = jiffies;
		if (!spacc->watchdog_armed && !READ_ONCE(spacc->shutting_down)) {
			spacc->watchdog_armed = true;
			mod_delayed_work(spacc->wq, &spacc->watchdog,
					 msecs_to_jiffies(1000));
		}
	}
	spacc->hw_inflight++;
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	rctx->offset += chunk;
	rctx->inflight_cmds++;

	return true;

sw_tail:
	rctx->sw_tail_len = left;
	if (rctx->inflight_cmds == 0) {
		int err = spacc_sw_crypt(ctx, slot->src, slot->dst, left,
					 rctx->iv, 0);

		if (!err)
			memcpy(req->iv, rctx->iv, AES_BLOCK_SIZE);
		spacc_finish_request(req, err);
	}
	return true;
}

static void spacc_work_fn(struct work_struct *work)
{
	struct spacc_dev *spacc = container_of(work, struct spacc_dev, work);

	spacc_reap_completed(spacc);

	while (((spacc->head + 1) % SPACC_RING_SIZE) != spacc->tail) {
		struct skcipher_request *target = NULL;
		struct spacc_reqctx *rctx;

		list_for_each_entry(rctx, &spacc->active_reqs, node) {
			struct skcipher_request *cand = rctx->req;

			if (!rctx->err && !rctx->sw_tail_len &&
			    rctx->offset < cand->cryptlen) {
				target = cand;
				break;
			}
		}

		if (!target) {
			struct crypto_async_request *areq, *backlog;

			spin_lock_bh(&spacc->q_lock);
			backlog = crypto_get_backlog(&spacc->queue);
			areq = crypto_dequeue_request(&spacc->queue);
			spin_unlock_bh(&spacc->q_lock);

			if (!areq)
				break;

			if (backlog) {
				local_bh_disable();
				backlog->complete(backlog, -EINPROGRESS);
				local_bh_enable();
			}

			target = skcipher_request_cast(areq);
			rctx = skcipher_request_ctx(target);
			list_add_tail(&rctx->node, &spacc->active_reqs);
		}

		if (!spacc_submit_next_chunk(spacc, target))
			break;

		if (((spacc->head + 1) % SPACC_RING_SIZE) == spacc->tail)
			spacc_reap_completed(spacc);
	}
}

static void spacc_watchdog_fn(struct work_struct *work)
{
	struct spacc_dev *spacc = container_of(work, struct spacc_dev,
					       watchdog.work);
	unsigned long flags;
	int i;

	if (READ_ONCE(spacc->shutting_down))
		return;

	spin_lock_irqsave(&spacc->hw_lock, flags);
	if (spacc->hw_inflight == 0) {
		spacc->watchdog_armed = false;
		spin_unlock_irqrestore(&spacc->hw_lock, flags);
		return;
	}

	if (!time_after(jiffies,
			spacc->last_progress + msecs_to_jiffies(1000))) {
		mod_delayed_work(spacc->wq, &spacc->watchdog,
				 msecs_to_jiffies(1000));
		spin_unlock_irqrestore(&spacc->hw_lock, flags);
		return;
	}

	spacc->resetting = true;
	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	dev_err_ratelimited(spacc->dev,
			    "SPACC: command stalled (IRQ_STAT=0x%08x FIFO_STAT=0x%08x), resetting\n",
			    readl(spacc->crypto_regs + SPACC_REG_IRQ_STAT),
			    readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT));

	synchronize_irq(spacc->irq);
	if (spacc->rst && !reset_control_assert(spacc->rst)) {
		fsleep(10);
		reset_control_deassert(spacc->rst);
	}

	spin_lock_irqsave(&spacc->hw_lock, flags);
	spacc_hw_init_regs(spacc);
	for (i = 0; i < SPACC_RING_SIZE; i++)
		spacc->ring[i].last_ctx = NULL;
	while (spacc->hw_inflight > 0) {
		struct spacc_hw_slot *slot = &spacc->ring[spacc->complete_idx];

		slot->err = -ETIMEDOUT;
		spacc->complete_idx = (spacc->complete_idx + 1) %
				      SPACC_RING_SIZE;
		spacc->hw_inflight--;
	}
	spacc->watchdog_armed = false;
	spacc->resetting = false;
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	queue_work(spacc->wq, &spacc->work);
}

static int spacc_cra_init(struct crypto_skcipher *tfm)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct spacc_alg *salg = container_of(alg, struct spacc_alg, alg);
	const char *name = crypto_tfm_alg_name(&tfm->base);

	ctx->spacc = salg->spacc;
	mutex_init(&ctx->lock);

	ctx->fallback_tfm = crypto_alloc_sync_skcipher(name, 0,
						       CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->fallback_tfm)) {
		dev_err(ctx->spacc->dev, "Cannot allocate fallback for %s\n",
			name);
		return PTR_ERR(ctx->fallback_tfm);
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
	memzero_explicit(ctx->key, sizeof(ctx->key));
	mutex_destroy(&ctx->lock);
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
	if (!err) {
		memzero_explicit(ctx->key, sizeof(ctx->key));
		memcpy(ctx->key, key, keylen);
		ctx->keylen = keylen;
		smp_wmb();
		WRITE_ONCE(ctx->key_gen,
			   (u32)atomic_inc_return(&spacc_key_gen_seq));
	}

	mutex_unlock(&ctx->lock);
	return err;
}

static int spacc_aes_crypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	struct spacc_dev *spacc = ctx->spacc;
	int err;

	if (req->cryptlen == 0)
		return 0;

	/*
	 * Short requests are cheaper in software.  Everything else is chunked by
	 * spacc_submit_next_chunk(): each chunk is trimmed to 15 source entries
	 * and to the 16-bit PROC_LEN field.
	 */
	if (req->cryptlen < SPACC_MIN_HW_BYTES)
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, req->base.flags);

	INIT_LIST_HEAD(&rctx->node);
	rctx->req = req;
	rctx->offset = 0;
	rctx->inflight_cmds = 0;
	rctx->sw_tail_len = 0;
	rctx->err = 0;
	memcpy(rctx->iv, req->iv, AES_BLOCK_SIZE);

	spin_lock_bh(&spacc->q_lock);
	err = crypto_enqueue_request(&spacc->queue, &req->base);
	spin_unlock_bh(&spacc->q_lock);

	if (err == -EBUSY && !(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG))
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, req->base.flags);

	queue_work(spacc->wq, &spacc->work);

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
	struct spacc_dev *spacc;
	struct resource *res;
	unsigned long flags;
	size_t ddt_stride, ddt_bytes;
	u32 spacc_id;
	int ret, i;

	spacc = devm_kzalloc(&pdev->dev, sizeof(*spacc), GFP_KERNEL);
	if (!spacc)
		return -ENOMEM;

	spacc->dev = &pdev->dev;
	spin_lock_init(&spacc->q_lock);
	spin_lock_init(&spacc->hw_lock);
	crypto_init_queue(&spacc->queue, SPACC_QUEUE_LEN);
	INIT_LIST_HEAD(&spacc->active_reqs);
	INIT_WORK(&spacc->work, spacc_work_fn);
	INIT_DELAYED_WORK(&spacc->watchdog, spacc_watchdog_fn);

	spacc->clk = devm_clk_get_optional(&pdev->dev, "spacc");
	if (IS_ERR(spacc->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(spacc->clk),
				     "Failed to get spacc clock\n");

	if (spacc->clk) {
		ret = clk_prepare_enable(spacc->clk);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "Failed to enable spacc clock\n");
	}

	spacc->rst = devm_reset_control_get_optional_shared(&pdev->dev,
							    "spacc");
	if (IS_ERR(spacc->rst)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(spacc->rst),
				    "Failed to get spacc reset\n");
		goto err_clk_disable;
	}

	if (spacc->rst) {
		ret = reset_control_deassert(spacc->rst);
		if (ret) {
			dev_err(&pdev->dev, "Failed to deassert reset: %d\n", ret);
			goto err_clk_disable;
		}
	}

	spacc->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(spacc->regs)) {
		ret = PTR_ERR(spacc->regs);
		goto err_reset_assert;
	}

	spacc->pdu_regs = spacc->regs;
	if (resource_size(res) >= SPACC_MIN_MMIO_SIZE)
		spacc->crypto_regs = spacc->regs + SPACC_CRYPTO_BASE_OFFSET;
	else
		spacc->crypto_regs = spacc->regs;

	spacc->cipher_ctx_base = spacc->crypto_regs + SPACC_REG_CIPH_KEY_BASE;

	spacc_id = readl(spacc->pdu_regs + SPACC_REG_ID);
	dev_info(&pdev->dev,
		 "LS1024A SPACC CTR engine probed (version %u.%u)\n",
		 (spacc_id >> 4) & 0x0f, spacc_id & 0x0f);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
		goto err_reset_assert;
	}

	ddt_stride = SPACC_MAX_DDT_ENTS * sizeof(struct spacc_ddt);
	ddt_bytes = SPACC_RING_SIZE * 2 * ddt_stride;
	spacc->ddt_pool = dmam_alloc_coherent(&pdev->dev, ddt_bytes,
					      &spacc->ddt_pool_dma, GFP_KERNEL);
	if (!spacc->ddt_pool) {
		ret = -ENOMEM;
		goto err_reset_assert;
	}

	for (i = 0; i < SPACC_RING_SIZE; i++) {
		struct spacc_hw_slot *slot = &spacc->ring[i];
		unsigned int base_idx = i * 2;

		slot->src_ddt = spacc->ddt_pool + base_idx * SPACC_MAX_DDT_ENTS;
		slot->src_ddt_dma = spacc->ddt_pool_dma + base_idx * ddt_stride;
		slot->dst_ddt = spacc->ddt_pool +
				(base_idx + 1) * SPACC_MAX_DDT_ENTS;
		slot->dst_ddt_dma = spacc->ddt_pool_dma +
				    (base_idx + 1) * ddt_stride;
	}

	/*
	 * One worker owns the slot ring and the hardware contexts, so max_active
	 * is 1 by design: pipeline depth comes from the ring.
	 */
	spacc->wq = alloc_workqueue("ls1024a-spacc",
				    WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!spacc->wq) {
		ret = -ENOMEM;
		goto err_reset_assert;
	}

	spacc->irq = platform_get_irq(pdev, 0);
	if (spacc->irq < 0) {
		ret = spacc->irq;
		goto err_wq_destroy;
	}

	spin_lock_irqsave(&spacc->hw_lock, flags);
	spacc_hw_init_regs(spacc);
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	ret = devm_request_irq(&pdev->dev, spacc->irq, spacc_irq_handler, 0,
			       dev_name(&pdev->dev), spacc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n",
			spacc->irq, ret);
		goto err_wq_destroy;
	}

	spacc->alg = spacc_alg_tmpl;
	spacc->alg.spacc = spacc;
	ret = crypto_register_skcipher(&spacc->alg.alg);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register %s: %d\n",
			spacc->alg.alg.base.cra_name, ret);
		goto err_irq_free;
	}

	platform_set_drvdata(pdev, spacc);
	return 0;

err_irq_free:
	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	devm_free_irq(&pdev->dev, spacc->irq, spacc);
err_wq_destroy:
	destroy_workqueue(spacc->wq);
err_reset_assert:
	if (spacc->rst)
		reset_control_assert(spacc->rst);
err_clk_disable:
	if (spacc->clk)
		clk_disable_unprepare(spacc->clk);
	return ret;
}

static void spacc_remove(struct platform_device *pdev)
{
	struct spacc_dev *spacc = platform_get_drvdata(pdev);
	struct spacc_reqctx *rctx, *tmp;
	struct crypto_async_request *areq;
	unsigned long flags;
	int i;

	crypto_unregister_skcipher(&spacc->alg.alg);

	/*
	 * Teardown order: block new IRQs, synchronize hard IRQ, cancel work and
	 * watchdog BEFORE touching ring[] or active_reqs so nothing can race.
	 */
	spacc->shutting_down = true;
	smp_mb();

	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	synchronize_irq(spacc->irq);

	/*
	 * Timer first: a running watchdog queues the worker on its way out, so
	 * cancelling the worker before the timer would leave that work pending.
	 */
	cancel_delayed_work_sync(&spacc->watchdog);
	cancel_work_sync(&spacc->work);

	/*
	 * If a command was running, a reset pulse stops its DMA before the
	 * buffers are released; the block is brought back so the context pages
	 * stay accessible for the wipe below.
	 */
	if (READ_ONCE(spacc->hw_inflight) && spacc->rst) {
		reset_control_assert(spacc->rst);
		udelay(20);
		reset_control_deassert(spacc->rst);
		udelay(20);
	}

	spin_lock_irqsave(&spacc->hw_lock, flags);
	for (i = 0; i < SPACC_RING_SIZE; i++)
		spacc->ring[i].req = NULL;
	spacc->head = 0;
	spacc->tail = 0;
	spacc->complete_idx = 0;
	spacc->hw_inflight = 0;
	spacc->watchdog_armed = false;
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	for (i = 0; i < SPACC_RING_SIZE; i++)
		spacc_unmap_slot(spacc, &spacc->ring[i]);

	list_for_each_entry_safe(rctx, tmp, &spacc->active_reqs, node)
		spacc_finish_request(rctx->req, -EIO);

	for (;;) {
		spin_lock_bh(&spacc->q_lock);
		areq = crypto_dequeue_request(&spacc->queue);
		spin_unlock_bh(&spacc->q_lock);

		if (!areq)
			break;
		spacc_finish_request(skcipher_request_cast(areq), -EIO);
	}

	devm_free_irq(&pdev->dev, spacc->irq, spacc);
	destroy_workqueue(spacc->wq);

	/* Key material survives cra_exit in the MMIO context pages: wipe. */
	for (i = 0; i < SPACC_RING_SIZE; i++)
		memset_io(spacc->cipher_ctx_base + (i * SPACC_CIPHER_PG_SZ),
			  0, SPACC_CIPHER_PG_SZ);

	if (spacc->rst)
		reset_control_assert(spacc->rst);

	if (spacc->clk)
		clk_disable_unprepare(spacc->clk);
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
MODULE_AUTHOR("Antigravity Project");
MODULE_LICENSE("GPL");