// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freescale LS1024A SPACC (Elliptic EPN-1802) Crypto Engine Driver
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/skcipher.h>
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
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

/* LS1024A PDU & SPACC Register Offsets */
#define SPACC_CRYPTO_BASE_OFFSET	0x00040000
/* Covers PDU registers + SPACC crypto window (0x40000..0x47fff) */
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
#define SPA_STATUS_ICV_FAIL		1
#define SPA_STATUS_MEMORY_ERROR		2
#define SPA_STATUS_BLOCK_ERROR		3
#define SPA_STATUS_SECURITY_ERROR	4
#define SPA_STATUS_RES_CODE_OFFSET	24
#define SPA_STATUS_RES_CODE_MASK	(0x7 << SPA_STATUS_RES_CODE_OFFSET)

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
#define SPA_KEY_SZ_CIPHER_OFFSET	31

/* Control register bits (EPN-1802 TRM) */
#define SPA_CTRL_CIPH_ALG_AES		0x02
#define SPA_CTRL_CIPH_MODE_ECB		(0x00 << 8)
#define SPA_CTRL_CIPH_MODE_CBC		(0x01 << 8)
#define SPA_CTRL_CIPH_MODE_CTR		(0x02 << 8)
#define SPA_CTRL_CTX_IDX		16
#define SPA_CTRL_ENCRYPT_IDX		24
/* Bit 29 overwrites the context slot key with the inverse AES key schedule */
#define SPA_CTRL_KEY_EXP		29

/*
 * LS1024A EPN-1802 hardware limits and erratum parameters:
 * Hardware DDT prefetcher supports up to 16 entries total (15 scatter-gather
 * segments + 1 null terminator entry).
 */
#define SPACC_MAX_DDT_ENTS		16
#define SPACC_MAX_SG_ENTS		(SPACC_MAX_DDT_ENTS - 1)
#define SPACC_RING_SIZE			16
#define SPACC_QUEUE_LEN			128
#define SPACC_CIPHER_PG_SZ		128
#define SPACC_CIPHER_IV_OFFSET		32
#define SPACC_IRQ_COALESCE_CNT		4

/*
 * PROC_LEN and DDT segment length fields are 16-bit (max 65535); round down
 * to AES block size (65520). Contiguous/coalesced SG entries reach 65520 B,
 * while discontiguous 4 KiB pages are capped at 15 * 4096 = 61440 B by
 * spacc_sg_span_cur().
 */
#define SPACC_MAX_CHUNK_BYTES		round_down(SZ_64K - 1, AES_BLOCK_SIZE)
/*
 * Below 2048 bytes (4096 bytes for non-pipelined CBC-encrypt), DDT setup +
 * IRQ + 64-byte tail recomputation overhead exceeds ARMv7 Cortex-A9 software
 * AES throughput; route small requests/tails directly to SW.
 */
#define SPACC_MIN_HW_BYTES		2048
#define SPACC_MIN_CBC_ENC_HW_BYTES	4096

/*
 * LS1024A EPN-1802 (0x18020642) erratum workaround:
 * Under memory contention, the last 8-24 bytes of a DMA write burst may be
 * corrupted. Recomputing the final 64 bytes (2 ARMv7 cache lines) on the CPU
 * after dma_unmap guarantees 100% integrity at full pipeline throughput.
 */
#define SPACC_TAIL_FIX_BYTES		64

static_assert(SPACC_TAIL_FIX_BYTES % AES_BLOCK_SIZE == 0);
static_assert(SPACC_MIN_HW_BYTES >= SPACC_TAIL_FIX_BYTES);
static_assert(SPACC_MIN_CBC_ENC_HW_BYTES >= SPACC_MIN_HW_BYTES);
static_assert(SPACC_MAX_CHUNK_BYTES >= SPACC_MIN_CBC_ENC_HW_BYTES);
static_assert(SPACC_CIPHER_PG_SZ >= SPACC_CIPHER_IV_OFFSET + AES_BLOCK_SIZE);
static_assert(SPACC_IRQ_COALESCE_CNT >= 1 &&
	      SPACC_IRQ_COALESCE_CNT <= SPACC_RING_SIZE / 2);

struct spacc_ddt {
	u32 p;
	u32 len;
};

struct spacc_dev;

struct spacc_alg {
	struct skcipher_alg alg;
	struct spacc_dev *spacc;
	u32 ctrl_default;
	bool is_ctr;
};

struct spacc_hw_slot {
	struct skcipher_request *req;
	/*
	 * Private bounded scatterlists clamped to exact chunk byte length so
	 * pipelined chunks of the same request never overlap in dma_map_sg().
	 */
	struct scatterlist src_sgl[SPACC_MAX_SG_ENTS];
	struct scatterlist dst_sgl[SPACC_MAX_SG_ENTS];
	struct scatterlist *src;
	struct scatterlist *dst;
	struct spacc_ddt *src_ddt;
	struct spacc_ddt *dst_ddt;
	dma_addr_t src_addr;
	dma_addr_t dst_addr;
	unsigned int src_nents;
	unsigned int dst_nents;
	unsigned int len;
	int err;
	u8 tail_iv[AES_BLOCK_SIZE] __aligned(4);
	u8 tail_src[SPACC_TAIL_FIX_BYTES] __aligned(4);

	/*
	 * Cached key state in hardware context slot_idx. Accessed exclusively
	 * by the single-threaded hw->wq worker while the slot is idle (between
	 * reap at hw->tail and submit at hw->head). Because KEY_EXP replaces
	 * the forward key in context memory with the inverse key schedule,
	 * both key generation and direction must match to skip reload.
	 */
	u64 cached_ctx_id;
	int cached_key_gen;
	unsigned int cached_keylen;
	bool cached_is_encrypt;
};

/*
 * Hardware-bound state whose lifetime strictly matches probe()..remove().
 * Published via RCU in struct spacc_dev; once unbound in remove() and
 * synchronize_rcu() completes, no TFM can obtain a pointer to spacc_hw,
 * cleanly decoupling devm-managed resources from kref-lived TFM contexts.
 */
struct spacc_hw {
	struct device *dev;
	void __iomem *pdu_regs;
	void __iomem *crypto_regs;
	void __iomem *cipher_ctx_base;
	int irq;
	struct reset_control *rst;
	struct dma_pool *req_pool;

	spinlock_t q_lock;
	struct crypto_queue queue;
	/* Owned exclusively by the single-threaded hw->wq worker */
	struct list_head active_reqs;

	spinlock_t hw_lock;
	struct spacc_hw_slot ring[SPACC_RING_SIZE];
	unsigned int head;
	unsigned int complete_idx;
	/* Owned exclusively by the single-threaded hw->wq worker */
	unsigned int tail;
	unsigned int hw_inflight;
	u32 irq_stat_cnt;
	unsigned long last_progress;

	struct workqueue_struct *wq;
	struct work_struct work;
	struct delayed_work watchdog;
	struct completion drained;
	bool stopping;
	bool skip_dma_unmap;
	bool halted_in_reset;
};

struct spacc_dev {
	struct kref refcount;
	struct spacc_hw __rcu *hw;
	atomic64_t next_ctx_id;
	struct spacc_alg algs[3];
};

struct spacc_ctx {
	struct spacc_dev *spacc;
	struct crypto_sync_skcipher *fallback_tfm;
	u64 ctx_id;
	atomic_t key_gen;
	u32 ctrl_default;
	u8 key[AES_MAX_KEY_SIZE] __aligned(4);
	unsigned int keylen;
	bool is_ctr;
	struct mutex lock;
};

struct spacc_reqctx {
	struct list_head node;
	struct skcipher_request *req;
	struct scatterlist *src_sg;
	struct scatterlist *dst_sg;
	unsigned int src_sg_off;
	unsigned int dst_sg_off;
	bool is_encrypt;
	bool cbc_enc_busy;
	bool sw_bridge_busy;
	bool completed;
	unsigned int offset;
	unsigned int inflight_cmds;
	unsigned int sw_tail_len;
	int err;
	u8 iv[AES_BLOCK_SIZE] __aligned(4);
	u8 final_iv[AES_BLOCK_SIZE] __aligned(4);
};

static bool spacc_use_hw = true;
module_param_named(hw, spacc_use_hw, bool, 0444);

static void spacc_dev_release(struct kref *kref)
{
	struct spacc_dev *spacc = container_of(kref, struct spacc_dev, refcount);

	kfree(spacc);
}

/*
 * EPN-1802 context memory maps key and IV bytes in native little-endian 32-bit
 * word order matching host byte stream layout (byte 0 at LSB of word 0).
 * Writes bytes to MMIO using relaxed stores; caller must order these writes
 * against subsequent device command register writes with writel().
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

/*
 * Writes all per-command fields using relaxed stores. Caller must ensure
 * ordering against the CTRL write via non-relaxed writel().
 */
static inline void spacc_write_proc_len(struct spacc_hw *hw, u32 len,
					u32 sw_id)
{
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_PRE_AAD_LEN);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_POST_AAD_LEN);
	writel_relaxed(len, hw->crypto_regs + SPACC_REG_PROC_LEN);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_ICV_LEN);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_ICV_OFFSET);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_IV_OFFSET);
	writel_relaxed(sw_id, hw->crypto_regs + SPACC_REG_SW_ID);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_AUX_INFO);
}

static inline void spacc_set_irq_coalesce(struct spacc_hw *hw, u32 cnt)
{
	lockdep_assert_held(&hw->hw_lock);

	if (hw->irq_stat_cnt != cnt) {
		writel(cnt << SPA_IRQ_CTRL_STAT_CNT_OFFSET,
		       hw->crypto_regs + SPACC_REG_IRQ_CTRL);
		hw->irq_stat_cnt = cnt;
	}
}

static int spacc_status_to_errno(u32 code)
{
	switch (code) {
	case SPA_STATUS_OK:
		return 0;
	case SPA_STATUS_ICV_FAIL:
		return -EBADMSG;
	case SPA_STATUS_MEMORY_ERROR:
		return -EFAULT;
	case SPA_STATUS_SECURITY_ERROR:
		return -EKEYREJECTED;
	case SPA_STATUS_BLOCK_ERROR:
		return -EIO;
	default:
		return -EINVAL;
	}
}

static inline void spacc_sg_normalize(struct scatterlist **sg,
				      unsigned int *off)
{
	while (*sg && *off >= (*sg)->length) {
		*off -= (*sg)->length;
		*sg = sg_next(*sg);
	}
}

static inline void spacc_sg_advance(struct scatterlist **sg,
				    unsigned int *off, unsigned int bytes)
{
	*off += bytes;
	spacc_sg_normalize(sg, off);
}

static unsigned int spacc_sg_span_cur(struct scatterlist *sg,
				      unsigned int sg_off,
				      unsigned int max_len,
				      unsigned int max_ents)
{
	unsigned int span = 0;
	unsigned int nents = 0;

	for (; sg && nents < max_ents && span < max_len; sg = sg_next(sg)) {
		unsigned int avail = sg->length - sg_off;

		sg_off = 0;
		if (!avail)
			continue;
		span += min(avail, max_len - span);
		nents++;
	}

	return span;
}

static unsigned int spacc_build_chunk_sgl_cur(struct scatterlist *dst_sgl,
					      struct scatterlist *src_sg,
					      unsigned int sg_off,
					      unsigned int target_bytes,
					      unsigned int max_ents,
					      unsigned int *out_nents)
{
	struct scatterlist *sg;
	unsigned int off, rem, nents = 0, i = 0;

	for (sg = src_sg, off = sg_off, rem = target_bytes;
	     sg && nents < max_ents && rem > 0; sg = sg_next(sg)) {
		unsigned int avail = sg->length - off;

		off = 0;
		if (!avail)
			continue;
		rem -= min(avail, rem);
		nents++;
	}

	if (rem != 0 || !nents) {
		*out_nents = 0;
		return 0;
	}

	sg_init_table(dst_sgl, nents);

	for (sg = src_sg, off = sg_off, rem = target_bytes;
	     sg && i < nents && rem > 0; sg = sg_next(sg)) {
		unsigned int avail = sg->length - off;
		unsigned int seg_len, full_off;

		if (!avail) {
			off = 0;
			continue;
		}
		seg_len = min(avail, rem);
		full_off = sg->offset + off;
		/*
		 * Block-layer / THP scatterlists can pack multi-page compound
		 * folios into a single sg entry with full_off >= PAGE_SIZE;
		 * advance the struct page pointer via nth_page() so page +
		 * offset stays normalized within a single base page.
		 */
		sg_set_page(&dst_sgl[i],
			    nth_page(sg_page(sg), full_off >> PAGE_SHIFT),
			    seg_len, full_off & ~PAGE_MASK);
		rem -= seg_len;
		off = 0;
		i++;
	}

	*out_nents = nents;
	return target_bytes;
}

static struct spacc_ddt *spacc_sg_to_ddt(struct spacc_hw *hw,
					 struct scatterlist *sgl,
					 unsigned int nbytes,
					 unsigned int nents,
					 enum dma_data_direction dir,
					 dma_addr_t *ddt_phys)
{
	struct spacc_ddt *ddt;
	struct scatterlist *cur;
	int mapped_ents, i, ddt_idx = 0;

	if (!nents || nents > SPACC_MAX_SG_ENTS)
		return NULL;

	mapped_ents = dma_map_sg(hw->dev, sgl, nents, dir);
	if (mapped_ents <= 0)
		return NULL;

	ddt = dma_pool_alloc(hw->req_pool, GFP_KERNEL, ddt_phys);
	if (!ddt) {
		dma_unmap_sg(hw->dev, sgl, nents, dir);
		return NULL;
	}

	/*
	 * dma_map_sg() may coalesce adjacent entries, returning
	 * mapped_ents <= nents. Iterate over mapped_ents DMA segments and
	 * enforce the SPACC_MAX_SG_ENTS hardware DDT limit.
	 */
	for_each_sg(sgl, cur, mapped_ents, i) {
		unsigned int len = min_t(unsigned int, sg_dma_len(cur), nbytes);

		if (!len)
			continue;
		if (ddt_idx >= SPACC_MAX_SG_ENTS) {
			dma_pool_free(hw->req_pool, ddt, *ddt_phys);
			dma_unmap_sg(hw->dev, sgl, nents, dir);
			return NULL;
		}
		ddt[ddt_idx].p = sg_dma_address(cur);
		ddt[ddt_idx].len = len;
		ddt_idx++;
		nbytes -= len;
		if (nbytes == 0)
			break;
	}

	if (nbytes != 0) {
		dma_pool_free(hw->req_pool, ddt, *ddt_phys);
		dma_unmap_sg(hw->dev, sgl, nents, dir);
		return NULL;
	}

	ddt[ddt_idx].p = 0;
	ddt[ddt_idx].len = 0;

	/* Order DDT writes before hardware starts prefetching descriptors */
	dma_wmb();

	return ddt;
}

static void spacc_ctr_add_blocks(u8 *iv, u32 blocks)
{
	u32 carry = 0;
	int i;

	for (i = 15; i >= 0 && (blocks || carry); i--) {
		u32 v = iv[i] + (blocks & 0xff) + carry;

		iv[i] = v & 0xff;
		carry = v >> 8;
		blocks >>= 8;
	}
}

static int spacc_sw_crypt(struct spacc_ctx *ctx, struct scatterlist *src,
			  struct scatterlist *dst, unsigned int len, u8 *iv,
			  bool is_encrypt, u32 flags)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(subreq, ctx->fallback_tfm);
	int err;

	/*
	 * Per-request state lives on the stack and the key inside fallback_tfm
	 * is immutable during active transforms per the Crypto API contract.
	 */
	skcipher_request_set_sync_tfm(subreq, ctx->fallback_tfm);
	skcipher_request_set_callback(subreq,
				      flags & ~CRYPTO_TFM_REQ_MAY_SLEEP,
				      NULL, NULL);
	skcipher_request_set_crypt(subreq, src, dst, len, iv);

	err = is_encrypt ? crypto_skcipher_encrypt(subreq) :
			   crypto_skcipher_decrypt(subreq);

	skcipher_request_zero(subreq);
	return err;
}

static int spacc_fixup_slot_tail(struct spacc_ctx *ctx,
				 struct spacc_reqctx *rctx,
				 struct spacc_hw_slot *slot,
				 unsigned int ivsize)
{
	u8 tail_out[SPACC_TAIL_FIX_BYTES] __aligned(4);
	struct scatterlist sg_in, sg_out;
	unsigned int fix_off = slot->len - SPACC_TAIL_FIX_BYTES;
	int err;

	if (!ctx->is_ctr && rctx->is_encrypt && ivsize)
		scatterwalk_map_and_copy(slot->tail_iv, slot->dst,
					 fix_off - ivsize, ivsize, 0);

	sg_init_one(&sg_in, slot->tail_src, SPACC_TAIL_FIX_BYTES);
	sg_init_one(&sg_out, tail_out, SPACC_TAIL_FIX_BYTES);

	err = spacc_sw_crypt(ctx, &sg_in, &sg_out, SPACC_TAIL_FIX_BYTES,
			     ivsize ? slot->tail_iv : NULL,
			     rctx->is_encrypt, 0);
	if (!err) {
		scatterwalk_map_and_copy(tail_out, slot->dst, fix_off,
					 SPACC_TAIL_FIX_BYTES, 1);
		if (ivsize) {
			if (!ctx->is_ctr && rctx->is_encrypt) {
				memcpy(rctx->iv,
				       tail_out + SPACC_TAIL_FIX_BYTES - ivsize,
				       ivsize);
				memcpy(rctx->final_iv, rctx->iv, ivsize);
			} else {
				/*
				 * spacc_sw_crypt() updates slot->tail_iv
				 * in-place per the skcipher API contract,
				 * leaving it at the exact post-chunk IV.
				 */
				memcpy(rctx->final_iv, slot->tail_iv, ivsize);
			}
		}
	}

	memzero_explicit(tail_out, sizeof(tail_out));
	memzero_explicit(slot->tail_src, sizeof(slot->tail_src));
	memzero_explicit(slot->tail_iv, sizeof(slot->tail_iv));
	return err;
}

static inline void spacc_invalidate_slot_cache(struct spacc_hw_slot *slot)
{
	slot->cached_ctx_id = 0;
	slot->cached_key_gen = 0;
	slot->cached_keylen = 0;
	slot->cached_is_encrypt = false;
}

static void __spacc_hw_init_regs(struct spacc_hw *hw, bool enable_irq)
{
	int limit = 64;
	int i;

	lockdep_assert_held(&hw->hw_lock);

	while (limit-- &&
	       !(readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		writel(~0, hw->crypto_regs + SPACC_REG_STAT_POP);
		(void)readl(hw->crypto_regs + SPACC_REG_STATUS);
	}

	writel(~0, hw->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(~0, hw->pdu_regs + SPACC_REG_IRQ_STAT);

	hw->irq_stat_cnt = 1;
	writel(1 << SPA_IRQ_CTRL_STAT_CNT_OFFSET,
	       hw->crypto_regs + SPACC_REG_IRQ_CTRL);
	writel(enable_irq ? (SPA_IRQ_EN_STAT_EN | SPA_IRQ_EN_GLBL_EN) : 0,
	       hw->pdu_regs + SPACC_REG_IRQ_EN);

	for (i = 0; i < SPACC_RING_SIZE; i++)
		spacc_invalidate_slot_cache(&hw->ring[i]);

	hw->hw_inflight = 0;
}

static void spacc_hw_init_regs(struct spacc_hw *hw)
{
	__spacc_hw_init_regs(hw, true);
}

/*
 * Mask interrupts, synchronize the IRQ handler, and assert hardware reset so
 * the bus master cannot continue writing to DMA buffers after unmap.
 */
static int spacc_halt_hw(struct spacc_hw *hw, bool deassert)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&hw->hw_lock, flags);
	writel(0, hw->pdu_regs + SPACC_REG_IRQ_EN);
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	synchronize_irq(hw->irq);

	ret = reset_control_assert(hw->rst);
	if (ret)
		return ret;

	if (deassert) {
		fsleep(10);
		ret = reset_control_deassert(hw->rst);
		if (ret)
			return ret;
	}

	return 0;
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

	while (hw->hw_inflight > 0 &&
	       !(readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		struct spacc_hw_slot *slot = &hw->ring[hw->complete_idx];

		writel(~0, hw->crypto_regs + SPACC_REG_STAT_POP);
		status = readl(hw->crypto_regs + SPACC_REG_STATUS);
		code = (status & SPA_STATUS_RES_CODE_MASK) >>
		       SPA_STATUS_RES_CODE_OFFSET;
		if (code != SPA_STATUS_OK)
			dev_warn_ratelimited(hw->dev,
					     "SPACC command failed: STATUS=0x%08x (code %u)\n",
					     status, code);

		slot->err = spacc_status_to_errno(code);
		hw->complete_idx = (hw->complete_idx + 1) % SPACC_RING_SIZE;
		hw->hw_inflight--;
		hw->last_progress = jiffies;
		completed_any = true;
	}

	/* Drop coalesce threshold when remaining in-flight commands < STAT_CNT */
	if (hw->hw_inflight < hw->irq_stat_cnt)
		spacc_set_irq_coalesce(hw, 1);

	/* Drain any orphaned status entries if hw_inflight is 0 to prevent IRQ storm */
	if (hw->hw_inflight == 0 &&
	    !(readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) &
	      SPA_FIFO_STAT_EMPTY)) {
		int limit = SPACC_RING_SIZE;

		dev_warn_ratelimited(hw->dev,
				     "Draining orphaned SPACC completion status\n");
		while (limit-- &&
		       !(readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) &
			 SPA_FIFO_STAT_EMPTY)) {
			writel(~0, hw->crypto_regs + SPACC_REG_STAT_POP);
			(void)readl(hw->crypto_regs + SPACC_REG_STATUS);
		}
	}

	/*
	 * Clear W1C latches after STAT_POP drains FIFO occupancy below STAT_CNT;
	 * clear the inner SPAcc core before the outer PDU wrapper so an active
	 * core line cannot immediately re-latch PDU IRQ_STAT.
	 */
	cstat = readl(hw->crypto_regs + SPACC_REG_IRQ_STAT);
	if (cstat)
		writel(cstat, hw->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(stat, hw->pdu_regs + SPACC_REG_IRQ_STAT);

	spin_unlock_irqrestore(&hw->hw_lock, flags);

	if (completed_any)
		queue_work(hw->wq, &hw->work);

	return IRQ_HANDLED;
}

static void spacc_free_slot_dma(struct spacc_hw *hw,
				struct spacc_hw_slot *slot)
{
	if (unlikely(READ_ONCE(hw->skip_dma_unmap))) {
		slot->src_ddt = NULL;
		slot->dst_ddt = NULL;
		return;
	}

	if (slot->src == slot->dst) {
		dma_unmap_sg(hw->dev, slot->src, slot->src_nents,
			     DMA_BIDIRECTIONAL);
		dma_pool_free(hw->req_pool, slot->src_ddt, slot->src_addr);
	} else {
		dma_unmap_sg(hw->dev, slot->src, slot->src_nents,
			     DMA_TO_DEVICE);
		dma_unmap_sg(hw->dev, slot->dst, slot->dst_nents,
			     DMA_FROM_DEVICE);
		dma_pool_free(hw->req_pool, slot->src_ddt, slot->src_addr);
		dma_pool_free(hw->req_pool, slot->dst_ddt, slot->dst_addr);
	}
	slot->src_ddt = NULL;
	slot->dst_ddt = NULL;
}

static void spacc_finish_request(struct skcipher_request *req, int err)
{
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);

	/*
	 * Must only be called once per request from the single-threaded
	 * hw->wq worker context.
	 */
	if (WARN_ON_ONCE(rctx->completed))
		return;
	rctx->completed = true;

	if (!list_empty(&rctx->node))
		list_del_init(&rctx->node);

	memzero_explicit(rctx->iv, sizeof(rctx->iv));
	memzero_explicit(rctx->final_iv, sizeof(rctx->final_iv));
	skcipher_request_complete(req, err);
}

/* Returns true if at least one ring slot was freed */
static bool spacc_reap_completed(struct spacc_hw *hw)
{
	unsigned long flags;
	unsigned int comp_idx;
	bool reaped_any = false;

	spin_lock_irqsave(&hw->hw_lock, flags);
	comp_idx = hw->complete_idx;
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	while (hw->tail != comp_idx) {
		struct spacc_hw_slot *slot = &hw->ring[hw->tail];
		struct skcipher_request *req = slot->req;
		struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
		struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
		struct spacc_reqctx *rctx = skcipher_request_ctx(req);
		unsigned int ivsize = crypto_skcipher_ivsize(tfm);
		/*
		 * Ordering of slot->err and complete_idx is guaranteed by the
		 * hw_lock acquire above pairing with the IRQ handler's release.
		 * dma_unmap_sg() ensures DMA visibility for the payload buffer.
		 */
		int slot_err = READ_ONCE(slot->err);

		spacc_free_slot_dma(hw, slot);

		if (slot_err) {
			spacc_invalidate_slot_cache(slot);
			if (!rctx->err)
				rctx->err = slot_err;
		}

		rctx->inflight_cmds--;
		if (rctx->inflight_cmds == 0) {
			rctx->cbc_enc_busy = false;
			rctx->sw_bridge_busy = false;
		}

		if (!rctx->err) {
			rctx->err = spacc_fixup_slot_tail(ctx, rctx, slot,
							  ivsize);
		} else {
			memzero_explicit(slot->tail_src,
					 sizeof(slot->tail_src));
			memzero_explicit(slot->tail_iv, sizeof(slot->tail_iv));
		}

		hw->tail = (hw->tail + 1) % SPACC_RING_SIZE;
		reaped_any = true;

		if (rctx->inflight_cmds == 0 &&
		    (rctx->err || rctx->sw_tail_len > 0 ||
		     rctx->offset == req->cryptlen)) {
			if (!rctx->err && rctx->sw_tail_len > 0) {
				struct scatterlist sc[2], dc[2];
				struct scatterlist *s, *d;

				s = scatterwalk_ffwd(sc, rctx->src_sg,
						     rctx->src_sg_off);
				d = (req->src == req->dst) ? s :
				    scatterwalk_ffwd(dc, rctx->dst_sg,
						     rctx->dst_sg_off);
				rctx->err = spacc_sw_crypt(ctx, s, d,
							   rctx->sw_tail_len,
							   rctx->final_iv,
							   rctx->is_encrypt,
							   req->base.flags);
			}

			if (!rctx->err && ivsize)
				memcpy(req->iv, rctx->final_iv, ivsize);

			spacc_finish_request(req, rctx->err);
		}
	}

	return reaped_any;
}

static void spacc_fallback_remainder(struct spacc_ctx *ctx,
				     struct spacc_reqctx *rctx,
				     struct skcipher_request *req,
				     unsigned int left, unsigned int ivsize)
{
	if (WARN_ON_ONCE(!left)) {
		if (rctx->inflight_cmds == 0)
			spacc_finish_request(req, rctx->err);
		return;
	}

	rctx->sw_tail_len = left;
	if (rctx->inflight_cmds == 0) {
		struct scatterlist sc[2], dc[2];
		struct scatterlist *s = scatterwalk_ffwd(sc, rctx->src_sg,
							 rctx->src_sg_off);
		struct scatterlist *d = (req->src == req->dst) ? s :
					scatterwalk_ffwd(dc, rctx->dst_sg,
							 rctx->dst_sg_off);
		int err = spacc_sw_crypt(ctx, s, d, left, rctx->final_iv,
					 rctx->is_encrypt, req->base.flags);
		if (!err && ivsize)
			memcpy(req->iv, rctx->final_iv, ivsize);
		spacc_finish_request(req, err);
	}
}

static bool spacc_bridge_sw_segment(struct spacc_ctx *ctx,
				    struct spacc_reqctx *rctx,
				    struct skcipher_request *req,
				    bool in_place,
				    unsigned int bytes, unsigned int ivsize)
{
	struct scatterlist sc[2], dc[2];
	struct scatterlist *s, *d;

	if (rctx->inflight_cmds > 0) {
		rctx->sw_bridge_busy = true;
		return true;
	}

	s = scatterwalk_ffwd(sc, rctx->src_sg, rctx->src_sg_off);
	d = in_place ? s : scatterwalk_ffwd(dc, rctx->dst_sg, rctx->dst_sg_off);

	rctx->err = spacc_sw_crypt(ctx, s, d, bytes, rctx->iv,
				   rctx->is_encrypt, req->base.flags);
	if (rctx->err) {
		spacc_finish_request(req, rctx->err);
		return true;
	}

	if (ivsize)
		memcpy(rctx->final_iv, rctx->iv, ivsize);
	rctx->offset += bytes;
	spacc_sg_advance(&rctx->src_sg, &rctx->src_sg_off, bytes);
	if (!in_place)
		spacc_sg_advance(&rctx->dst_sg, &rctx->dst_sg_off, bytes);
	return true;
}

static bool spacc_submit_next_chunk(struct spacc_hw *hw,
				    struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	unsigned int ivsize = crypto_skcipher_ivsize(tfm);
	bool in_place = (req->src == req->dst);
	unsigned int min_hw_bytes = (!ctx->is_ctr && rctx->is_encrypt && ivsize) ?
				    SPACC_MIN_CBC_ENC_HW_BYTES :
				    SPACC_MIN_HW_BYTES;
	unsigned int left = req->cryptlen - rctx->offset;
	unsigned int slot_idx = hw->head;
	struct spacc_hw_slot *slot = &hw->ring[slot_idx];
	unsigned int chunk, src_nents = 0, dst_nents = 0;
	u8 key_copy[AES_MAX_KEY_SIZE] __aligned(4) = { 0 };
	u8 next_iv[AES_BLOCK_SIZE] __aligned(4) = { 0 };
	unsigned int keylen;
	void __iomem *page;
	unsigned long flags;
	bool key_cached;
	int key_gen;
	u32 ctrl;

	if (left < min_hw_bytes) {
		spacc_fallback_remainder(ctx, rctx, req, left, ivsize);
		return true;
	}

	chunk = min_t(unsigned int, left, SPACC_MAX_CHUNK_BYTES);
	if (ctx->is_ctr) {
		/*
		 * EPN-1802 hardware AES-CTR increments only the lower 16 bits
		 * of the counter block without carry into upper bytes. Split
		 * chunks at the 16-bit counter wrap boundary; if the wrap
		 * distance is smaller than min_hw_bytes, wait for prior
		 * in-flight commands to drain before bridging in SW so
		 * final_iv updates remain strictly monotonic.
		 */
		u32 low = ((u32)rctx->iv[14] << 8) | rctx->iv[15];
		u64 limit = (0x10000ULL - low) * AES_BLOCK_SIZE;

		if (limit < min_hw_bytes && limit < left)
			return spacc_bridge_sw_segment(ctx, rctx, req, in_place,
						       (unsigned int)limit,
						       ivsize);

		if (limit < chunk)
			chunk = (unsigned int)limit;
	}

	chunk = spacc_sg_span_cur(rctx->src_sg, rctx->src_sg_off, chunk,
				  SPACC_MAX_SG_ENTS);
	if (!in_place)
		chunk = spacc_sg_span_cur(rctx->dst_sg, rctx->dst_sg_off, chunk,
					  SPACC_MAX_SG_ENTS);

	/*
	 * Align intermediate chunk boundaries to 2 AES blocks (32 bytes = ARMv7
	 * Cortex-A9 cache line size) to avoid cache-line sharing between
	 * concurrent DMA chunks; round final chunk to AES block size so
	 * unaligned CTR tails go through spacc_fallback_remainder().
	 */
	if (chunk < left)
		chunk = round_down(chunk, 2 * AES_BLOCK_SIZE);
	else
		chunk = round_down(chunk, AES_BLOCK_SIZE);

	if (chunk < min_hw_bytes) {
		if (chunk > 0 && left - chunk >= min_hw_bytes)
			return spacc_bridge_sw_segment(ctx, rctx, req, in_place,
						       chunk, ivsize);

		spacc_fallback_remainder(ctx, rctx, req, left, ivsize);
		return true;
	}

	/* Build exact-length SGLs so dma_map_sg() never maps past chunk */
	if (spacc_build_chunk_sgl_cur(slot->src_sgl, rctx->src_sg,
				      rctx->src_sg_off, chunk,
				      SPACC_MAX_SG_ENTS,
				      &src_nents) != chunk) {
		spacc_fallback_remainder(ctx, rctx, req, left, ivsize);
		return true;
	}
	slot->src = slot->src_sgl;

	if (in_place) {
		slot->dst = slot->src_sgl;
		dst_nents = src_nents;
	} else {
		if (spacc_build_chunk_sgl_cur(slot->dst_sgl, rctx->dst_sg,
					      rctx->dst_sg_off, chunk,
					      SPACC_MAX_SG_ENTS,
					      &dst_nents) != chunk) {
			spacc_fallback_remainder(ctx, rctx, req, left, ivsize);
			return true;
		}
		slot->dst = slot->dst_sgl;
	}

	/* Snapshot tail data and next-chunk IV before DMA mapping */
	scatterwalk_map_and_copy(slot->tail_src, slot->src,
				 chunk - SPACC_TAIL_FIX_BYTES,
				 SPACC_TAIL_FIX_BYTES, 0);
	if (ivsize) {
		if (ctx->is_ctr) {
			memcpy(slot->tail_iv, rctx->iv, ivsize);
			spacc_ctr_add_blocks(slot->tail_iv,
					     (chunk - SPACC_TAIL_FIX_BYTES) /
					     AES_BLOCK_SIZE);
			memcpy(next_iv, rctx->iv, ivsize);
			spacc_ctr_add_blocks(next_iv, chunk / AES_BLOCK_SIZE);
		} else if (!rctx->is_encrypt) {
			scatterwalk_map_and_copy(slot->tail_iv, slot->src,
						 chunk - SPACC_TAIL_FIX_BYTES - ivsize,
						 ivsize, 0);
			scatterwalk_map_and_copy(next_iv, slot->src,
						 chunk - ivsize, ivsize, 0);
		}
	}

	if (in_place) {
		slot->src_ddt = spacc_sg_to_ddt(hw, slot->src, chunk,
						src_nents, DMA_BIDIRECTIONAL,
						&slot->src_addr);
		if (!slot->src_ddt) {
			memzero_explicit(next_iv, sizeof(next_iv));
			memzero_explicit(slot->tail_src,
					 sizeof(slot->tail_src));
			memzero_explicit(slot->tail_iv, sizeof(slot->tail_iv));
			spacc_fallback_remainder(ctx, rctx, req, left, ivsize);
			return true;
		}
		slot->dst_ddt = slot->src_ddt;
		slot->dst_addr = slot->src_addr;
	} else {
		slot->src_ddt = spacc_sg_to_ddt(hw, slot->src, chunk,
						src_nents, DMA_TO_DEVICE,
						&slot->src_addr);
		if (!slot->src_ddt) {
			memzero_explicit(next_iv, sizeof(next_iv));
			memzero_explicit(slot->tail_src,
					 sizeof(slot->tail_src));
			memzero_explicit(slot->tail_iv, sizeof(slot->tail_iv));
			spacc_fallback_remainder(ctx, rctx, req, left, ivsize);
			return true;
		}
		slot->dst_ddt = spacc_sg_to_ddt(hw, slot->dst, chunk,
						dst_nents, DMA_FROM_DEVICE,
						&slot->dst_addr);
		if (!slot->dst_ddt) {
			memzero_explicit(next_iv, sizeof(next_iv));
			memzero_explicit(slot->tail_src,
					 sizeof(slot->tail_src));
			memzero_explicit(slot->tail_iv, sizeof(slot->tail_iv));
			dma_unmap_sg(hw->dev, slot->src, src_nents,
				     DMA_TO_DEVICE);
			dma_pool_free(hw->req_pool, slot->src_ddt,
				      slot->src_addr);
			slot->src_ddt = NULL;
			spacc_fallback_remainder(ctx, rctx, req, left, ivsize);
			return true;
		}
	}

	slot->req = req;
	slot->src_nents = src_nents;
	slot->dst_nents = dst_nents;
	slot->len = chunk;
	slot->err = 0;

	/* Slot index maps 1:1 to hardware cipher context 0..15 */
	page = hw->cipher_ctx_base + (slot_idx * SPACC_CIPHER_PG_SZ);
	key_gen = atomic_read(&ctx->key_gen);
	key_cached = (slot->cached_ctx_id == ctx->ctx_id &&
		      slot->cached_key_gen == key_gen &&
		      slot->cached_is_encrypt == rctx->is_encrypt);
	if (!key_cached) {
		mutex_lock(&ctx->lock);
		key_gen = atomic_read(&ctx->key_gen);
		keylen = ctx->keylen;
		memcpy(key_copy, ctx->key, keylen);
		mutex_unlock(&ctx->lock);

		spacc_memcpy_toio32(page, key_copy, keylen);
		memzero_explicit(key_copy, sizeof(key_copy));
		slot->cached_ctx_id = ctx->ctx_id;
		slot->cached_key_gen = key_gen;
		slot->cached_keylen = keylen;
		slot->cached_is_encrypt = rctx->is_encrypt;
	} else {
		keylen = slot->cached_keylen;
	}

	if (ivsize)
		spacc_memcpy_toio32(page + SPACC_CIPHER_IV_OFFSET, rctx->iv,
				    ivsize);

	ctrl = ctx->ctrl_default | (slot_idx << SPA_CTRL_CTX_IDX);
	if (rctx->is_encrypt)
		ctrl |= BIT(SPA_CTRL_ENCRYPT_IDX);
	else if (!key_cached)
		ctrl |= BIT(SPA_CTRL_KEY_EXP);

	/*
	 * Context writes and command registers target the same Device MMIO
	 * mapping, and DDT memory was already ordered by dma_wmb(). The
	 * non-relaxed writel(ctrl) below issues __iowmb() before starting DMA.
	 */
	spin_lock_irqsave(&hw->hw_lock, flags);
	if (hw->hw_inflight >= SPACC_RING_SIZE ||
	    (readl(hw->crypto_regs + SPACC_REG_FIFO_STAT) &
	     SPA_FIFO_CMD_FULL)) {
		spin_unlock_irqrestore(&hw->hw_lock, flags);
		memzero_explicit(next_iv, sizeof(next_iv));
		memzero_explicit(slot->tail_src, sizeof(slot->tail_src));
		memzero_explicit(slot->tail_iv, sizeof(slot->tail_iv));
		spacc_free_slot_dma(hw, slot);
		spacc_invalidate_slot_cache(slot);
		return false;
	}

	writel_relaxed(keylen | (slot_idx << SPA_KEY_SZ_CTX_INDEX_OFFSET) |
		       (1U << SPA_KEY_SZ_CIPHER_OFFSET),
		       hw->crypto_regs + SPACC_REG_KEY_SZ);
	writel_relaxed(slot->src_addr, hw->crypto_regs + SPACC_REG_SRC_PTR);
	writel_relaxed(slot->dst_addr, hw->crypto_regs + SPACC_REG_DST_PTR);
	writel_relaxed(0, hw->crypto_regs + SPACC_REG_OFFSET);
	spacc_write_proc_len(hw, chunk, slot_idx);
	writel(ctrl, hw->crypto_regs + SPACC_REG_CTRL);

	hw->head = (hw->head + 1) % SPACC_RING_SIZE;
	if (hw->hw_inflight == 0)
		hw->last_progress = jiffies;
	hw->hw_inflight++;
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	/* Commit request progression only after hardware submission succeeds */
	if (ivsize && (ctx->is_ctr || !rctx->is_encrypt))
		memcpy(rctx->iv, next_iv, ivsize);
	memzero_explicit(next_iv, sizeof(next_iv));

	rctx->offset += chunk;
	spacc_sg_advance(&rctx->src_sg, &rctx->src_sg_off, chunk);
	if (!in_place)
		spacc_sg_advance(&rctx->dst_sg, &rctx->dst_sg_off, chunk);

	rctx->inflight_cmds++;
	if (ivsize && !ctx->is_ctr && rctx->is_encrypt)
		rctx->cbc_enc_busy = true;

	return true;
}

static void spacc_drain_stopping(struct spacc_hw *hw)
{
	struct spacc_reqctx *rctx, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&hw->hw_lock, flags);
	spacc_set_irq_coalesce(hw, 1);
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	/*
	 * Complete any active requests whose DMA commands have already finished
	 * using the software fallback so callers never hang during remove().
	 * Requests with inflight_cmds > 0 stay on active_reqs until
	 * spacc_reap_completed() drains their final IRQ/forced completion.
	 */
	list_for_each_entry_safe(rctx, tmp, &hw->active_reqs, node) {
		struct skcipher_request *req = rctx->req;
		struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
		struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
		unsigned int ivsize = crypto_skcipher_ivsize(tfm);
		unsigned int left = req->cryptlen - rctx->offset;

		if (rctx->inflight_cmds != 0)
			continue;

		if (!rctx->err && left > 0) {
			struct scatterlist sc[2], dc[2];
			struct scatterlist *s = scatterwalk_ffwd(sc,
								 rctx->src_sg,
								 rctx->src_sg_off);
			struct scatterlist *d = (req->src == req->dst) ? s :
						scatterwalk_ffwd(dc,
								 rctx->dst_sg,
								 rctx->dst_sg_off);

			rctx->err = spacc_sw_crypt(ctx, s, d, left,
						   rctx->final_iv,
						   rctx->is_encrypt,
						   req->base.flags);
		}

		if (!rctx->err && ivsize)
			memcpy(req->iv, rctx->final_iv, ivsize);

		spacc_finish_request(req, rctx->err);
	}

	for (;;) {
		struct crypto_async_request *areq, *backlog;
		struct skcipher_request *req;
		struct crypto_skcipher *tfm;
		struct spacc_ctx *ctx;
		struct spacc_reqctx *rctx;
		int err;

		spin_lock_bh(&hw->q_lock);
		backlog = crypto_get_backlog(&hw->queue);
		areq = crypto_dequeue_request(&hw->queue);
		spin_unlock_bh(&hw->q_lock);

		if (!areq)
			break;

		if (backlog)
			backlog->complete(backlog, -EINPROGRESS);

		req = skcipher_request_cast(areq);
		tfm = crypto_skcipher_reqtfm(req);
		ctx = crypto_skcipher_ctx(tfm);
		rctx = skcipher_request_ctx(req);

		err = spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				     req->iv, rctx->is_encrypt,
				     req->base.flags);
		spacc_finish_request(req, err);
	}

	if (list_empty(&hw->active_reqs))
		complete_all(&hw->drained);
}

static void spacc_work_fn(struct work_struct *work)
{
	struct spacc_hw *hw = container_of(work, struct spacc_hw, work);
	int budget = SPACC_RING_SIZE;
	unsigned long flags;
	unsigned int inflight;
	bool reaped_any;

	/*
	 * Loop while reaping frees up ring slots so the hardware pipeline stays
	 * saturated, bounded by budget so spacc_watchdog_fn() (on the same
	 * max_active=1 workqueue) can never be starved under continuous load.
	 */
	do {
		spacc_reap_completed(hw);

		if (unlikely(READ_ONCE(hw->stopping))) {
			spacc_drain_stopping(hw);
			cancel_delayed_work(&hw->watchdog);
			return;
		}

		while (((hw->head + 1) % SPACC_RING_SIZE) != hw->tail &&
		       !READ_ONCE(hw->stopping)) {
			struct skcipher_request *target = NULL;
			struct spacc_reqctx *rctx;

			/* Exclusive access: hw->wq is single-threaded (max_active=1) */
			list_for_each_entry(rctx, &hw->active_reqs, node) {
				struct skcipher_request *cand = rctx->req;

				if (!rctx->err && !rctx->sw_tail_len &&
				    !rctx->cbc_enc_busy &&
				    !rctx->sw_bridge_busy &&
				    rctx->offset < cand->cryptlen) {
					target = cand;
					break;
				}
			}

			if (!target) {
				struct crypto_async_request *areq, *backlog;

				spin_lock_bh(&hw->q_lock);
				backlog = crypto_get_backlog(&hw->queue);
				areq = crypto_dequeue_request(&hw->queue);
				spin_unlock_bh(&hw->q_lock);

				if (!areq)
					break;

				if (backlog)
					backlog->complete(backlog, -EINPROGRESS);

				target = skcipher_request_cast(areq);
				rctx = skcipher_request_ctx(target);
				list_add_tail(&rctx->node, &hw->active_reqs);
			}

			if (!spacc_submit_next_chunk(hw, target))
				break;
		}

		reaped_any = spacc_reap_completed(hw);
	} while (reaped_any && --budget > 0 && !READ_ONCE(hw->stopping));

	if (unlikely(READ_ONCE(hw->stopping))) {
		spacc_drain_stopping(hw);
		cancel_delayed_work(&hw->watchdog);
		return;
	}

	spin_lock_irqsave(&hw->hw_lock, flags);
	inflight = hw->hw_inflight;
	if (inflight >= SPACC_IRQ_COALESCE_CNT)
		spacc_set_irq_coalesce(hw, SPACC_IRQ_COALESCE_CNT);
	else
		spacc_set_irq_coalesce(hw, 1);
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	/* Arm lazy watchdog only if not already pending to avoid timer lock churn */
	if (inflight > 0 && !delayed_work_pending(&hw->watchdog))
		queue_delayed_work(hw->wq, &hw->watchdog,
				   msecs_to_jiffies(1000));

	/* Re-queue ourselves if budget expired with pending work remaining */
	if (budget == 0) {
		bool has_queued;

		spin_lock_bh(&hw->q_lock);
		has_queued = hw->queue.qlen > 0;
		spin_unlock_bh(&hw->q_lock);

		if (has_queued || !list_empty(&hw->active_reqs))
			queue_work(hw->wq, &hw->work);
	}
}

static void spacc_watchdog_fn(struct work_struct *work)
{
	struct spacc_hw *hw = container_of(work, struct spacc_hw,
					    watchdog.work);
	unsigned long flags, last_prog, deadline;
	bool active;
	int ret;

	if (READ_ONCE(hw->stopping))
		return;

	spin_lock_irqsave(&hw->hw_lock, flags);
	active = (hw->hw_inflight > 0);
	last_prog = hw->last_progress;
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	if (!active)
		return;

	deadline = last_prog + msecs_to_jiffies(1000);
	if (time_before(jiffies, deadline)) {
		if (!READ_ONCE(hw->stopping))
			queue_delayed_work(hw->wq, &hw->watchdog,
					   deadline - jiffies);
		return;
	}

	dev_err_ratelimited(hw->dev,
			    "SPACC stalled (IRQ_STAT=0x%08x FIFO_STAT=0x%08x), resetting\n",
			    readl(hw->crypto_regs + SPACC_REG_IRQ_STAT),
			    readl(hw->crypto_regs + SPACC_REG_FIFO_STAT));

	ret = spacc_halt_hw(hw, true);
	if (ret) {
		dev_crit(hw->dev,
			 "Failed to reset stalled SPACC (%d); halting HW path and leaking active DMA mappings\n",
			 ret);
		WRITE_ONCE(hw->skip_dma_unmap, true);
		WRITE_ONCE(hw->stopping, true);
		spin_lock_irqsave(&hw->hw_lock, flags);
		hw->hw_inflight = 0;
		while (hw->complete_idx != hw->head) {
			hw->ring[hw->complete_idx].err = -ETIMEDOUT;
			hw->complete_idx = (hw->complete_idx + 1) %
					   SPACC_RING_SIZE;
		}
		spin_unlock_irqrestore(&hw->hw_lock, flags);
		queue_work(hw->wq, &hw->work);
		return;
	}

	/*
	 * Hardware reset discards any unpopped status FIFO entries; fail all
	 * in-flight ring slots with -ETIMEDOUT so upper layers can retry.
	 */
	spin_lock_irqsave(&hw->hw_lock, flags);
	spacc_hw_init_regs(hw);
	while (hw->complete_idx != hw->head) {
		hw->ring[hw->complete_idx].err = -ETIMEDOUT;
		hw->complete_idx = (hw->complete_idx + 1) % SPACC_RING_SIZE;
	}
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	queue_work(hw->wq, &hw->work);
}

static int spacc_cra_init(struct crypto_skcipher *tfm)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct spacc_alg *spacc_alg = container_of(alg, struct spacc_alg, alg);
	struct spacc_dev *spacc = spacc_alg->spacc;
	const char *name = crypto_tfm_alg_name(&tfm->base);

	if (!kref_get_unless_zero(&spacc->refcount))
		return -ENODEV;

	if (!rcu_access_pointer(spacc->hw)) {
		kref_put(&spacc->refcount, spacc_dev_release);
		return -ENODEV;
	}

	ctx->fallback_tfm = crypto_alloc_sync_skcipher(name, 0,
						       CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->fallback_tfm)) {
		kref_put(&spacc->refcount, spacc_dev_release);
		return PTR_ERR(ctx->fallback_tfm);
	}

	ctx->spacc = spacc;
	ctx->ctx_id = atomic64_inc_return(&spacc->next_ctx_id);
	atomic_set(&ctx->key_gen, 0);
	ctx->ctrl_default = spacc_alg->ctrl_default;
	ctx->is_ctr = spacc_alg->is_ctr;
	mutex_init(&ctx->lock);

	crypto_skcipher_set_reqsize(tfm, sizeof(struct spacc_reqctx));
	return 0;
}

static void spacc_cra_exit(struct crypto_skcipher *tfm)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);

	crypto_free_sync_skcipher(ctx->fallback_tfm);
	memzero_explicit(ctx->key, sizeof(ctx->key));
	mutex_destroy(&ctx->lock);
	kref_put(&ctx->spacc->refcount, spacc_dev_release);
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

	crypto_sync_skcipher_clear_flags(ctx->fallback_tfm,
					 CRYPTO_TFM_REQ_MASK);
	crypto_sync_skcipher_set_flags(ctx->fallback_tfm,
				       crypto_skcipher_get_flags(tfm) &
				       CRYPTO_TFM_REQ_MASK);

	err = crypto_sync_skcipher_setkey(ctx->fallback_tfm, key, keylen);
	if (!err) {
		memzero_explicit(ctx->key, sizeof(ctx->key));
		memcpy(ctx->key, key, keylen);
		ctx->keylen = keylen;
		/* Full barrier in atomic_inc_return publishes key before key_gen */
		atomic_inc_return(&ctx->key_gen);
	}

	mutex_unlock(&ctx->lock);
	return err;
}

/*
 * Returns -EINPROGRESS when queued for asynchronous HW execution, -EBUSY when
 * queued into backlog, or 0 / negative errno when completed inline via SW.
 */
static int spacc_aes_crypt(struct skcipher_request *req, bool is_encrypt)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	struct spacc_dev *spacc = ctx->spacc;
	unsigned int ivsize = crypto_skcipher_ivsize(tfm);
	unsigned int min_hw_bytes = (!ctx->is_ctr && is_encrypt && ivsize) ?
				    SPACC_MIN_CBC_ENC_HW_BYTES :
				    SPACC_MIN_HW_BYTES;
	struct spacc_hw *hw;
	int err;

	if (req->cryptlen == 0)
		return 0;

	if (!ctx->is_ctr && (req->cryptlen % AES_BLOCK_SIZE))
		return -EINVAL;

	rcu_read_lock();
	hw = rcu_dereference(spacc->hw);
	if (unlikely(!hw || READ_ONCE(hw->stopping) ||
		     !READ_ONCE(spacc_use_hw) ||
		     req->cryptlen < min_hw_bytes)) {
		rcu_read_unlock();
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, is_encrypt, req->base.flags);
	}

	INIT_LIST_HEAD(&rctx->node);
	rctx->req = req;
	rctx->src_sg = req->src;
	rctx->src_sg_off = 0;
	spacc_sg_normalize(&rctx->src_sg, &rctx->src_sg_off);
	if (req->src == req->dst) {
		rctx->dst_sg = rctx->src_sg;
		rctx->dst_sg_off = rctx->src_sg_off;
	} else {
		rctx->dst_sg = req->dst;
		rctx->dst_sg_off = 0;
		spacc_sg_normalize(&rctx->dst_sg, &rctx->dst_sg_off);
	}
	rctx->is_encrypt = is_encrypt;
	rctx->cbc_enc_busy = false;
	rctx->sw_bridge_busy = false;
	rctx->completed = false;
	rctx->offset = 0;
	rctx->inflight_cmds = 0;
	rctx->sw_tail_len = 0;
	rctx->err = 0;
	if (ivsize) {
		memcpy(rctx->iv, req->iv, ivsize);
		memcpy(rctx->final_iv, req->iv, ivsize);
	}

	spin_lock_bh(&hw->q_lock);
	err = crypto_enqueue_request(&hw->queue, &req->base);
	spin_unlock_bh(&hw->q_lock);

	if (err == -EINPROGRESS || err == -EBUSY)
		queue_work(hw->wq, &hw->work);
	rcu_read_unlock();

	return err;
}

static int spacc_aes_ecb_encrypt(struct skcipher_request *req)
{
	return spacc_aes_crypt(req, true);
}

static int spacc_aes_ecb_decrypt(struct skcipher_request *req)
{
	return spacc_aes_crypt(req, false);
}

static int spacc_aes_cbc_encrypt(struct skcipher_request *req)
{
	return spacc_aes_crypt(req, true);
}

static int spacc_aes_cbc_decrypt(struct skcipher_request *req)
{
	return spacc_aes_crypt(req, false);
}

static int spacc_aes_ctr_crypt(struct skcipher_request *req)
{
	return spacc_aes_crypt(req, true);
}

static const struct spacc_alg spacc_algs_tmpl[] = {
	{
		.ctrl_default = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_CTR,
		.is_ctr = true,
		.alg = {
			.base.cra_name		= "ctr(aes)",
			.base.cra_driver_name	= "ctr-aes-ls1024a-spacc",
			.base.cra_priority	= 500,
			.base.cra_flags		= CRYPTO_ALG_ASYNC |
						  CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize	= 1,
			.base.cra_ctxsize	= sizeof(struct spacc_ctx),
			.base.cra_module	= THIS_MODULE,

			.init			= spacc_cra_init,
			.exit			= spacc_cra_exit,
			.min_keysize		= AES_MIN_KEY_SIZE,
			.max_keysize		= AES_MAX_KEY_SIZE,
			.ivsize			= AES_BLOCK_SIZE,
			.setkey			= spacc_aes_setkey,
			.encrypt		= spacc_aes_ctr_crypt,
			.decrypt		= spacc_aes_ctr_crypt,
		},
	},
	{
		.ctrl_default = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_ECB,
		.alg = {
			.base.cra_name		= "ecb(aes)",
			.base.cra_driver_name	= "ecb-aes-ls1024a-spacc",
			.base.cra_priority	= 500,
			.base.cra_flags		= CRYPTO_ALG_ASYNC |
						  CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize	= AES_BLOCK_SIZE,
			.base.cra_ctxsize	= sizeof(struct spacc_ctx),
			.base.cra_module	= THIS_MODULE,

			.init			= spacc_cra_init,
			.exit			= spacc_cra_exit,
			.min_keysize		= AES_MIN_KEY_SIZE,
			.max_keysize		= AES_MAX_KEY_SIZE,
			.setkey			= spacc_aes_setkey,
			.encrypt		= spacc_aes_ecb_encrypt,
			.decrypt		= spacc_aes_ecb_decrypt,
		},
	},
	{
		.ctrl_default = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_CBC,
		.alg = {
			.base.cra_name		= "cbc(aes)",
			.base.cra_driver_name	= "cbc-aes-ls1024a-spacc",
			.base.cra_priority	= 500,
			.base.cra_flags		= CRYPTO_ALG_ASYNC |
						  CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize	= AES_BLOCK_SIZE,
			.base.cra_ctxsize	= sizeof(struct spacc_ctx),
			.base.cra_module	= THIS_MODULE,

			.init			= spacc_cra_init,
			.exit			= spacc_cra_exit,
			.min_keysize		= AES_MIN_KEY_SIZE,
			.max_keysize		= AES_MAX_KEY_SIZE,
			.ivsize			= AES_BLOCK_SIZE,
			.setkey			= spacc_aes_setkey,
			.encrypt		= spacc_aes_cbc_encrypt,
			.decrypt		= spacc_aes_cbc_decrypt,
		},
	},
};

static void spacc_quiesce_hw(struct platform_device *pdev,
			     struct spacc_dev *spacc, struct spacc_hw *hw)
{
	unsigned long flags;

	WRITE_ONCE(hw->stopping, true);
	RCU_INIT_POINTER(spacc->hw, NULL);
	synchronize_rcu();

	/*
	 * After cancel_delayed_work_sync() returns, watchdog will not issue
	 * any new resets; watchdog may have already queued hw->work, making
	 * the subsequent queue_work() an idempotent no-op.
	 */
	cancel_delayed_work_sync(&hw->watchdog);
	queue_work(hw->wq, &hw->work);

	if (!wait_for_completion_timeout(&hw->drained,
					 msecs_to_jiffies(2000))) {
		int ret = spacc_halt_hw(hw, false);

		if (ret) {
			dev_crit(hw->dev,
				 "Failed to halt SPACC during teardown (%d); leaking active DMA mappings\n",
				 ret);
			WRITE_ONCE(hw->skip_dma_unmap, true);
		} else {
			hw->halted_in_reset = true;
		}

		spin_lock_irqsave(&hw->hw_lock, flags);
		hw->hw_inflight = 0;
		while (hw->complete_idx != hw->head) {
			hw->ring[hw->complete_idx].err = -EIO;
			hw->complete_idx = (hw->complete_idx + 1) %
					   SPACC_RING_SIZE;
		}
		spin_unlock_irqrestore(&hw->hw_lock, flags);

		queue_work(hw->wq, &hw->work);
		if (!wait_for_completion_timeout(&hw->drained,
						 msecs_to_jiffies(2000)))
			dev_err(hw->dev,
				"Timed out draining remaining requests\n");
	}

	if (!hw->halted_in_reset) {
		spin_lock_irqsave(&hw->hw_lock, flags);
		writel(0, hw->pdu_regs + SPACC_REG_IRQ_EN);
		spin_unlock_irqrestore(&hw->hw_lock, flags);
	}

	/* Ensure IRQ handler has exited before destroying hw->wq */
	devm_free_irq(&pdev->dev, hw->irq, hw);

	drain_workqueue(hw->wq);
	destroy_workqueue(hw->wq);
}

static int spacc_probe(struct platform_device *pdev)
{
	struct spacc_dev *spacc;
	struct spacc_hw *hw;
	struct resource *res;
	struct clk *clk;
	unsigned long flags;
	u32 spacc_id;
	int ret, i;

	spacc = kzalloc(sizeof(*spacc), GFP_KERNEL);
	if (!spacc)
		return -ENOMEM;

	hw = devm_kzalloc(&pdev->dev, sizeof(*hw), GFP_KERNEL);
	if (!hw) {
		kfree(spacc);
		return -ENOMEM;
	}

	kref_init(&spacc->refcount);
	hw->dev = &pdev->dev;
	spin_lock_init(&hw->q_lock);
	spin_lock_init(&hw->hw_lock);
	crypto_init_queue(&hw->queue, SPACC_QUEUE_LEN);
	INIT_LIST_HEAD(&hw->active_reqs);
	INIT_WORK(&hw->work, spacc_work_fn);
	INIT_DELAYED_WORK(&hw->watchdog, spacc_watchdog_fn);
	init_completion(&hw->drained);

	clk = devm_clk_get_enabled(&pdev->dev, "spacc");
	if (IS_ERR(clk)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(clk),
				    "Failed to enable spacc clock\n");
		goto err_put_ref;
	}

	hw->rst = devm_reset_control_get_exclusive(&pdev->dev, "spacc");
	if (IS_ERR(hw->rst)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(hw->rst),
				    "Failed to get spacc reset\n");
		goto err_put_ref;
	}

	ret = reset_control_deassert(hw->rst);
	if (ret) {
		dev_err(&pdev->dev, "Failed to deassert reset: %d\n", ret);
		goto err_put_ref;
	}

	hw->pdu_regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hw->pdu_regs)) {
		ret = PTR_ERR(hw->pdu_regs);
		goto err_reset_assert;
	}

	if (resource_size(res) < SPACC_MIN_MMIO_SIZE) {
		dev_err(&pdev->dev, "Invalid MMIO resource %pr\n", res);
		ret = -EINVAL;
		goto err_reset_assert;
	}

	hw->crypto_regs = hw->pdu_regs + SPACC_CRYPTO_BASE_OFFSET;
	hw->cipher_ctx_base = hw->crypto_regs + SPACC_REG_CIPH_KEY_BASE;

	spacc_id = readl(hw->pdu_regs + SPACC_REG_ID);
	dev_info(&pdev->dev, "LS1024A SPACC engine probed (version %u.%u)\n",
		 (spacc_id >> 4) & 0x0f, spacc_id & 0x0f);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
		goto err_reset_assert;
	}
	/* Keep each coalesced SG segment within the 16-bit DDT length field */
	ret = dma_set_max_seg_size(&pdev->dev, SPACC_MAX_CHUNK_BYTES);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set max DMA segment size: %d\n",
			ret);
		goto err_reset_assert;
	}

	hw->req_pool = dmam_pool_create(dev_name(&pdev->dev), &pdev->dev,
					SPACC_MAX_DDT_ENTS * sizeof(struct spacc_ddt),
					8, 0);
	if (!hw->req_pool) {
		ret = -ENOMEM;
		goto err_reset_assert;
	}

	hw->wq = alloc_workqueue(DRV_NAME,
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

	/* Request IRQ before unmasking hardware interrupts */
	ret = devm_request_irq(&pdev->dev, hw->irq, spacc_irq_handler, 0,
			       dev_name(&pdev->dev), hw);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n",
			hw->irq, ret);
		goto err_wq_destroy;
	}

	spin_lock_irqsave(&hw->hw_lock, flags);
	spacc_hw_init_regs(hw);
	spin_unlock_irqrestore(&hw->hw_lock, flags);

	rcu_assign_pointer(spacc->hw, hw);

	for (i = 0; i < ARRAY_SIZE(spacc_algs_tmpl); i++) {
		spacc->algs[i] = spacc_algs_tmpl[i];
		spacc->algs[i].spacc = spacc;
		ret = crypto_register_skcipher(&spacc->algs[i].alg);
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to register algorithm %s: %d\n",
				spacc->algs[i].alg.base.cra_name, ret);
			while (--i >= 0)
				crypto_unregister_skcipher(&spacc->algs[i].alg);
			goto err_quiesce_hw;
		}
	}

	platform_set_drvdata(pdev, spacc);
	return 0;

err_quiesce_hw:
	spacc_quiesce_hw(pdev, spacc, hw);
	goto err_reset_assert;
err_wq_destroy:
	/* Safe without drain: spacc->hw was never published, no work queued */
	destroy_workqueue(hw->wq);
err_reset_assert:
	if (!hw->halted_in_reset)
		reset_control_assert(hw->rst);
err_put_ref:
	kref_put(&spacc->refcount, spacc_dev_release);
	return ret;
}

static void spacc_remove(struct platform_device *pdev)
{
	struct spacc_dev *spacc = platform_get_drvdata(pdev);
	struct spacc_hw *hw = rcu_dereference_protected(spacc->hw, 1);
	int i;

	for (i = 0; i < ARRAY_SIZE(spacc->algs); i++)
		crypto_unregister_skcipher(&spacc->algs[i].alg);

	spacc_quiesce_hw(pdev, spacc, hw);

	if (!hw->halted_in_reset) {
		for (i = 0; i < SPACC_RING_SIZE; i++)
			memset_io(hw->cipher_ctx_base +
				  (i * SPACC_CIPHER_PG_SZ),
				  0, SPACC_CIPHER_PG_SZ);

		reset_control_assert(hw->rst);
	}

	kref_put(&spacc->refcount, spacc_dev_release);
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
		.name		= DRV_NAME,
		.of_match_table	= spacc_of_match,
	},
};
module_platform_driver(spacc_driver);

MODULE_DESCRIPTION("Freescale LS1024A SPACC Crypto Engine Driver");
MODULE_AUTHOR("Antigravity Team");
MODULE_LICENSE("GPL");
