// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freescale LS1024A SPACC Crypto Engine Driver (Pipelined Ring Edition)
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/skcipher.h>
#include <linux/bottom_half.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
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

/* Hardware Register Offsets */
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

/* Control register bits */
#define SPA_CTRL_CIPH_ALG_AES		0x02
#define SPA_CTRL_CIPH_MODE_ECB		(0x00 << 8)
#define SPA_CTRL_CIPH_MODE_CBC		(0x01 << 8)
#define SPA_CTRL_CIPH_MODE_CTR		(0x02 << 8)
#define SPA_CTRL_CTX_IDX		16
#define SPA_CTRL_ENCRYPT_IDX		24
#define SPA_CTRL_KEY_EXP		29

/* Engine limits and pipeline tuning */
/* Hardware DDT prefetcher stops after 16 entries (15 data + 1 terminator) */
#define SPACC_MAX_DDT_ENTS		16
/*
 * One command covers at most 15 data entries: the DDT prefetcher drops the
 * 16th.  Entry count is NOT what corrupts data under load (proved: 4-entry
 * commands fail just as well); memory bus contention is.  Single-threaded
 * use is clean at any count up to this limit.
 */
#define SPACC_MAX_SG_ENTS		(SPACC_MAX_DDT_ENTS - 1)
#define SPACC_RING_SIZE			16
#define SPACC_QUEUE_LEN			128
#define SPACC_CIPHER_PG_SZ		128
#define SPACC_CIPHER_IV_OFFSET		32
#define SPACC_CRYPTO_BASE_OFFSET	0x00040000
#define SPACC_MIN_MMIO_SIZE		0x00048000

#define SPACC_MAX_CHUNK_BYTES		round_down(SZ_64K - 1, AES_BLOCK_SIZE)
#define SPACC_MIN_HW_BYTES		(8 * 1024)
/*
 * A short remainder of an already-running request is still cheaper on the
 * engine than a fallback: 4 KiB on the engine costs ~0.17 ms, the same on
 * aes-generic ~1.6 ms.  The whole-request entry threshold stays at 8 KiB.
 */
#define SPACC_MIN_TAIL_HW_BYTES		2048

/*
 * Descriptor as the engine sees it.  The address is 32-bit because the
 * driver sets a 32-bit DMA mask in probe(); widening it would need both a
 * larger descriptor and a different mask.
 */
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
	struct scatterlist src_chain[2];
	struct scatterlist dst_chain[2];
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
	u8 next_iv[AES_BLOCK_SIZE] __aligned(4);
};

struct spacc_dev {
	struct device *dev;
	void __iomem *regs;
	void __iomem *pdu_regs;
	void __iomem *crypto_regs;
	void __iomem *cipher_ctx_base;
	unsigned int cipher_pg_sz;
	int irq;
	struct clk *clk;
	struct reset_control *rst;
	struct dma_pool *req_pool;

	/* Asynchronous request backlog */
	spinlock_t q_lock;
	struct crypto_queue queue;
	struct list_head active_reqs;

	/* Hardware command pipeline ring (1:1 slot -> hardware ctx_id 0..15) */
	spinlock_t hw_lock;
	struct spacc_hw_slot ring[SPACC_RING_SIZE];
	unsigned int head;		/* next slot to submit (worker only) */
	unsigned int complete_idx;	/* next slot to pop status (ISR) */
	unsigned int tail;		/* oldest slot to unmap/finalize (worker) */
	unsigned int hw_inflight;	/* commands currently in HW FIFOs */
	unsigned long last_progress;

	struct workqueue_struct *wq;
	struct work_struct work;
	struct delayed_work watchdog;
	bool shutting_down;

	struct spacc_alg algs[3];
};

struct spacc_ctx {
	struct spacc_dev *spacc;
	struct crypto_sync_skcipher *fallback_tfm;
	u32 ctrl_default;
	u8 key[AES_MAX_KEY_SIZE] __aligned(4);
	unsigned int keylen;
	bool is_ctr;
	struct mutex lock;
};

struct spacc_reqctx {
	struct list_head node;
	struct skcipher_request *req;
	bool is_encrypt;
	bool cbc_enc_busy;
	unsigned int offset;
	unsigned int inflight_cmds;
	unsigned int sw_tail_len;
	int err;
	u8 iv[AES_BLOCK_SIZE] __aligned(4);
	u8 final_iv[AES_BLOCK_SIZE] __aligned(4);
};

static inline void spacc_memcpy_toio32(void __iomem *dst, const void *src,
				       unsigned int bytes)
{
	u32 __iomem *dst32 = dst;
	const u8 *src8 = src;
	unsigned int words = bytes / 4;
	unsigned int i;

	WARN_ON_ONCE(bytes % 4);

	for (i = 0; i < words; i++) {
		u32 val = get_unaligned((const u32 *)(src8 + i * 4));

		writel(val, dst32 + i);
	}
}

static inline void spacc_write_proc_len(struct spacc_dev *spacc, u32 len,
					u32 sw_id)
{
	writel(0, spacc->crypto_regs + SPACC_REG_PRE_AAD_LEN);
	writel(0, spacc->crypto_regs + SPACC_REG_POST_AAD_LEN);
	writel(len, spacc->crypto_regs + SPACC_REG_PROC_LEN);
	writel(0, spacc->crypto_regs + SPACC_REG_ICV_LEN);
	writel(0, spacc->crypto_regs + SPACC_REG_ICV_OFFSET);
	writel(0, spacc->crypto_regs + SPACC_REG_IV_OFFSET);
	writel(sw_id, spacc->crypto_regs + SPACC_REG_SW_ID);
	writel(0, spacc->crypto_regs + SPACC_REG_AUX_INFO);
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
		return -EACCES;
	case SPA_STATUS_BLOCK_ERROR:
		return -EIO;
	default:
		return -EINVAL;
	}
}

static unsigned int spacc_sg_span(struct scatterlist *sg, unsigned int len,
				  unsigned int max_ents,
				  unsigned int *out_nents)
{
	unsigned int span = 0;
	unsigned int nents = 0;

	for (; sg && nents < max_ents && span < len; sg = sg_next(sg)) {
		span += min(sg->length, len - span);
		nents++;
	}

	if (out_nents)
		*out_nents = nents;

	return span;
}

static struct spacc_ddt *spacc_sg_to_ddt(struct spacc_dev *spacc,
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

	mapped_ents = dma_map_sg(spacc->dev, sgl, nents, dir);
	if (mapped_ents <= 0)
		return NULL;

	ddt = dma_pool_alloc(spacc->req_pool, GFP_KERNEL, ddt_phys);
	if (!ddt) {
		dma_unmap_sg(spacc->dev, sgl, nents, dir);
		return NULL;
	}

	for_each_sg(sgl, cur, mapped_ents, i) {
		unsigned int len = min_t(unsigned int, sg_dma_len(cur), nbytes);

		if (!len)
			continue;
		ddt[ddt_idx].p = sg_dma_address(cur);
		ddt[ddt_idx].len = len;
		ddt_idx++;
		nbytes -= len;
		if (nbytes == 0)
			break;
	}
	ddt[ddt_idx].p = 0;
	ddt[ddt_idx].len = 0;

	/*
	 * LS1024A erratum workaround: the DDT prefetcher may race descriptor
	 * writes.  On this platform a read back from coherent memory drains the
	 * write path for the region before the MMIO CTRL write below can start
	 * the prefetcher.  The kernel does not guarantee this ordering on other
	 * architectures; that is why the platform is named here.
	 */
	(void)READ_ONCE(ddt[ddt_idx].len);

	dma_wmb();

	return ddt;
}

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
			  bool is_encrypt, u32 flags)
{
	int err;

	if (unlikely(!ctx->fallback_tfm))
		return -EINVAL;

	{
		SYNC_SKCIPHER_REQUEST_ON_STACK(subreq, ctx->fallback_tfm);

		skcipher_request_set_sync_tfm(subreq, ctx->fallback_tfm);
		skcipher_request_set_callback(subreq,
					      flags & CRYPTO_TFM_REQ_MAY_SLEEP,
					      NULL, NULL);
		skcipher_request_set_crypt(subreq, src, dst, len, iv);

		if (is_encrypt)
			err = crypto_skcipher_encrypt(subreq);
		else
			err = crypto_skcipher_decrypt(subreq);

		skcipher_request_zero(subreq);
	}

	return err;
}

static void spacc_hw_init_regs(struct spacc_dev *spacc)
{
	int limit = 64;

	lockdep_assert_held(&spacc->hw_lock);

	while (limit-- &&
	       !(readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		writel(~0, spacc->crypto_regs + SPACC_REG_STAT_POP);
		(void)readl(spacc->crypto_regs + SPACC_REG_STATUS);
	}

	writel(~0, spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(~0, spacc->pdu_regs + SPACC_REG_IRQ_STAT);

	/* Keep STAT_CNT fixed at 1 forever to avoid IRQ threshold glitches */
	writel(1 << SPA_IRQ_CTRL_STAT_CNT_OFFSET,
	       spacc->crypto_regs + SPACC_REG_IRQ_CTRL);
	writel(SPA_IRQ_EN_STAT_EN | SPA_IRQ_EN_GLBL_EN,
	       spacc->pdu_regs + SPACC_REG_IRQ_EN);

	spacc->hw_inflight = 0;
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

		writel(~0, spacc->crypto_regs + SPACC_REG_STAT_POP);
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

static void spacc_free_slot_dma(struct spacc_dev *spacc,
				struct spacc_hw_slot *slot)
{
	if (slot->src == slot->dst) {
		dma_unmap_sg(spacc->dev, slot->src, slot->src_nents,
			     DMA_BIDIRECTIONAL);
		dma_pool_free(spacc->req_pool, slot->src_ddt, slot->src_addr);
	} else {
		dma_unmap_sg(spacc->dev, slot->src, slot->src_nents,
			     DMA_TO_DEVICE);
		dma_unmap_sg(spacc->dev, slot->dst, slot->dst_nents,
			     DMA_FROM_DEVICE);
		dma_pool_free(spacc->req_pool, slot->src_ddt, slot->src_addr);
		dma_pool_free(spacc->req_pool, slot->dst_ddt, slot->dst_addr);
	}
	slot->src_ddt = NULL;
	slot->dst_ddt = NULL;
}

static void spacc_finish_request(struct skcipher_request *req, int err)
{
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);

	list_del_init(&rctx->node);
	memzero_explicit(rctx->iv, sizeof(rctx->iv));
	memzero_explicit(rctx->final_iv, sizeof(rctx->final_iv));
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
		unsigned int ivsize = crypto_skcipher_ivsize(tfm);
		int slot_err = slot->err;

		spacc_free_slot_dma(spacc, slot);
		dma_rmb();

		if (slot_err && !rctx->err)
			rctx->err = slot_err;

		rctx->inflight_cmds--;
		rctx->cbc_enc_busy = false;

		if (ivsize && !rctx->err) {
			if (!ctx->is_ctr && rctx->is_encrypt) {
				/*
				 * The next chunk chains from this ciphertext block, so it
				 * has to become the running IV, not just the final one.
				 */
				scatterwalk_map_and_copy(rctx->iv, slot->dst,
							 slot->len - ivsize,
							 ivsize, 0);
				memcpy(rctx->final_iv, rctx->iv, ivsize);
			} else {
				memcpy(rctx->final_iv, slot->next_iv, ivsize);
			}
		}

		spacc->tail = (spacc->tail + 1) % SPACC_RING_SIZE;

		if (rctx->inflight_cmds == 0 &&
		    (rctx->err || rctx->sw_tail_len > 0 ||
		     rctx->offset == req->cryptlen)) {
			if (!rctx->err && rctx->sw_tail_len > 0) {
				struct scatterlist sc[2], dc[2];
				struct scatterlist *s, *d;

				s = scatterwalk_ffwd(sc, req->src, rctx->offset);
				d = (req->src == req->dst) ? s :
				    scatterwalk_ffwd(dc, req->dst, rctx->offset);
				rctx->err = spacc_sw_crypt(ctx, s, d,
							   rctx->sw_tail_len,
							   rctx->final_iv,
							   rctx->is_encrypt, 0);
			}

			if (!rctx->err && ivsize)
				memcpy(req->iv, rctx->final_iv, ivsize);

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
	unsigned int ivsize = crypto_skcipher_ivsize(tfm);
	bool in_place = (req->src == req->dst);
	unsigned int left = req->cryptlen - rctx->offset;
	unsigned int slot_idx = spacc->head;
	struct spacc_hw_slot *slot = &spacc->ring[slot_idx];
	unsigned int chunk, src_nents = 0, dst_nents = 0;
	void __iomem *page;
	unsigned long flags;
	u32 ctrl;

	/*
	 * One command at a time.  Concurrent load on the memory bus is the
	 * trigger of the silicon write-path defect, and serialization costs
	 * nothing measured.  The ring stays: completion bookkeeping, watchdog
	 * recovery and hardware-context indexing still run through it; only its
	 * depth is unused while commands do not overlap.
	 */
	if (READ_ONCE(spacc->hw_inflight) > 0)
		return false;

	if (left < SPACC_MIN_TAIL_HW_BYTES) {
		rctx->sw_tail_len = left;
		if (rctx->inflight_cmds == 0) {
			struct scatterlist sc[2], dc[2];
			struct scatterlist *s, *d;
			int err;

			s = scatterwalk_ffwd(sc, req->src, rctx->offset);
			d = in_place ? s :
			    scatterwalk_ffwd(dc, req->dst, rctx->offset);
			err = spacc_sw_crypt(ctx, s, d, left, rctx->final_iv,
					     rctx->is_encrypt, 0);
			if (!err && ivsize)
				memcpy(req->iv, rctx->final_iv, ivsize);
			spacc_finish_request(req, err);
		}
		return true;
	}

	slot->src = scatterwalk_ffwd(slot->src_chain, req->src, rctx->offset);
	slot->dst = in_place ? slot->src :
		    scatterwalk_ffwd(slot->dst_chain, req->dst, rctx->offset);

	chunk = min_t(unsigned int, left, SPACC_MAX_CHUNK_BYTES);
	if (ctx->is_ctr) {
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
	/*
	 * The chunk end must not split a cache line: DMA is non-coherent,
	 * and a dirty line covering [end - 16, end + 16) would clobber the
	 * last block written by the engine.
	 */
	chunk = round_down(chunk, 2 * AES_BLOCK_SIZE);

	if (chunk < SPACC_MIN_TAIL_HW_BYTES) {
		rctx->sw_tail_len = left;
		if (rctx->inflight_cmds == 0) {
			int err = spacc_sw_crypt(ctx, slot->src, slot->dst,
						 left, rctx->final_iv,
						 rctx->is_encrypt, 0);
			if (!err && ivsize)
				memcpy(req->iv, rctx->final_iv, ivsize);
			spacc_finish_request(req, err);
		}
		return true;
	}

	if (in_place) {
		slot->src_ddt = spacc_sg_to_ddt(spacc, slot->src, chunk,
						src_nents, DMA_BIDIRECTIONAL,
						&slot->src_addr);
		if (!slot->src_ddt) {
			rctx->sw_tail_len = left;
			if (rctx->inflight_cmds == 0) {
				int err = spacc_sw_crypt(ctx, slot->src,
							 slot->dst, left,
							 rctx->final_iv,
							 rctx->is_encrypt, 0);
				if (!err && ivsize)
					memcpy(req->iv, rctx->final_iv, ivsize);
				spacc_finish_request(req, err);
			}
			return true;
		}
		slot->dst_ddt = slot->src_ddt;
		slot->dst_addr = slot->src_addr;
	} else {
		slot->src_ddt = spacc_sg_to_ddt(spacc, slot->src, chunk,
						src_nents, DMA_TO_DEVICE,
						&slot->src_addr);
		if (!slot->src_ddt) {
			rctx->sw_tail_len = left;
			if (rctx->inflight_cmds == 0) {
				int err = spacc_sw_crypt(ctx, slot->src,
							 slot->dst, left,
							 rctx->final_iv,
							 rctx->is_encrypt, 0);
				if (!err && ivsize)
					memcpy(req->iv, rctx->final_iv, ivsize);
				spacc_finish_request(req, err);
			}
			return true;
		}
		slot->dst_ddt = spacc_sg_to_ddt(spacc, slot->dst, chunk,
						dst_nents, DMA_FROM_DEVICE,
						&slot->dst_addr);
		if (!slot->dst_ddt) {
			dma_unmap_sg(spacc->dev, slot->src, src_nents,
				     DMA_TO_DEVICE);
			dma_pool_free(spacc->req_pool, slot->src_ddt,
				      slot->src_addr);
			slot->src_ddt = NULL;
			rctx->sw_tail_len = left;
			if (rctx->inflight_cmds == 0) {
				int err = spacc_sw_crypt(ctx, slot->src,
							 slot->dst, left,
							 rctx->final_iv,
							 rctx->is_encrypt, 0);
				if (!err && ivsize)
					memcpy(req->iv, rctx->final_iv, ivsize);
				spacc_finish_request(req, err);
			}
			return true;
		}
	}

	slot->req = req;
	slot->src_nents = src_nents;
	slot->dst_nents = dst_nents;
	slot->len = chunk;
	slot->err = 0;

	/* Slot index maps 1:1 to hardware cipher context 0..15 */
	page = spacc->cipher_ctx_base + (slot_idx * spacc->cipher_pg_sz);
	spacc_memcpy_toio32(page, ctx->key, ctx->keylen);
	if (ivsize)
		spacc_memcpy_toio32(page + SPACC_CIPHER_IV_OFFSET, rctx->iv,
				    ivsize);
	(void)readl(page);

	if (ctx->is_ctr) {
		spacc_ctr_advance(rctx->iv, chunk);
		if (ivsize)
			memcpy(slot->next_iv, rctx->iv, ivsize);
	} else if (!rctx->is_encrypt && ivsize) {
		scatterwalk_map_and_copy(rctx->iv, slot->src, chunk - ivsize,
					 ivsize, 0);
		memcpy(slot->next_iv, rctx->iv, ivsize);
	}

	ctrl = ctx->ctrl_default | (slot_idx << SPA_CTRL_CTX_IDX);
	if (rctx->is_encrypt)
		ctrl |= BIT(SPA_CTRL_ENCRYPT_IDX);
	else
		ctrl |= BIT(SPA_CTRL_KEY_EXP);

	spin_lock_irqsave(&spacc->hw_lock, flags);
	if (readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) &
	    SPA_FIFO_CMD_FULL) {
		spin_unlock_irqrestore(&spacc->hw_lock, flags);
		spacc_free_slot_dma(spacc, slot);
		return false;
	}

	writel(ctx->keylen | (slot_idx << SPA_KEY_SZ_CTX_INDEX_OFFSET) |
	       (1U << SPA_KEY_SZ_CIPHER_OFFSET),
	       spacc->crypto_regs + SPACC_REG_KEY_SZ);
	writel(slot->src_addr, spacc->crypto_regs + SPACC_REG_SRC_PTR);
	writel(slot->dst_addr, spacc->crypto_regs + SPACC_REG_DST_PTR);
	writel(0, spacc->crypto_regs + SPACC_REG_OFFSET);
	spacc_write_proc_len(spacc, chunk, slot_idx);
	writel(ctrl, spacc->crypto_regs + SPACC_REG_CTRL);

	spacc->head = (spacc->head + 1) % SPACC_RING_SIZE;
	if (spacc->hw_inflight == 0)
		spacc->last_progress = jiffies;
	spacc->hw_inflight++;
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	rctx->offset += chunk;
	rctx->inflight_cmds++;
	rctx->cbc_enc_busy = true;

	return true;
}

static void spacc_work_fn(struct work_struct *work)
{
	struct spacc_dev *spacc = container_of(work, struct spacc_dev, work);
	unsigned long flags;
	unsigned int inflight;

	spacc_reap_completed(spacc);

	while (((spacc->head + 1) % SPACC_RING_SIZE) != spacc->tail) {
		struct skcipher_request *target = NULL;
		struct spacc_reqctx *rctx;

		list_for_each_entry(rctx, &spacc->active_reqs, node) {
			struct skcipher_request *cand = rctx->req;

			if (!rctx->err && !rctx->sw_tail_len &&
			    !rctx->cbc_enc_busy &&
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

		/* Reap any fast completions while feeding the pipeline */
		spacc_reap_completed(spacc);
	}

	spin_lock_irqsave(&spacc->hw_lock, flags);
	inflight = spacc->hw_inflight;
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	if (inflight > 0 && !spacc->shutting_down)
		mod_delayed_work(spacc->wq, &spacc->watchdog,
				 msecs_to_jiffies(1000));
	else
		cancel_delayed_work(&spacc->watchdog);
}

static void spacc_watchdog_fn(struct work_struct *work)
{
	struct spacc_dev *spacc = container_of(work, struct spacc_dev,
					       watchdog.work);
	unsigned long flags;
	bool timed_out = false;

	if (READ_ONCE(spacc->shutting_down))
		return;

	spin_lock_irqsave(&spacc->hw_lock, flags);
	if (spacc->hw_inflight > 0 &&
	    time_after(jiffies, spacc->last_progress + msecs_to_jiffies(1000)))
		timed_out = true;
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	if (!timed_out)
		return;

	dev_err_ratelimited(spacc->dev,
			    "SPACC: pipeline stalled (IRQ_STAT=0x%08x FIFO_STAT=0x%08x), resetting engine\n",
			    readl(spacc->crypto_regs + SPACC_REG_IRQ_STAT),
			    readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT));

	spin_lock_irqsave(&spacc->hw_lock, flags);
	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	synchronize_irq(spacc->irq);
	if (spacc->rst && !reset_control_assert(spacc->rst)) {
		fsleep(10);
		reset_control_deassert(spacc->rst);
	}

	spin_lock_irqsave(&spacc->hw_lock, flags);
	spacc_hw_init_regs(spacc);
	/*
	 * hw_inflight is not decremented here: spacc_hw_init_regs() above has
	 * already reset it to 0 together with the engine state.
	 */
	while (spacc->complete_idx != spacc->head) {
		spacc->ring[spacc->complete_idx].err = -ETIMEDOUT;
		spacc->complete_idx = (spacc->complete_idx + 1) % SPACC_RING_SIZE;
	}
	spin_unlock_irqrestore(&spacc->hw_lock, flags);

	queue_work(spacc->wq, &spacc->work);
}

static int spacc_cra_init(struct crypto_skcipher *tfm)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct spacc_alg *spacc_alg = container_of(alg, struct spacc_alg, alg);
	const char *name = crypto_tfm_alg_name(&tfm->base);

	ctx->spacc = spacc_alg->spacc;
	ctx->ctrl_default = spacc_alg->ctrl_default;
	ctx->is_ctr = spacc_alg->is_ctr;
	mutex_init(&ctx->lock);

	if (!ctx->is_ctr) {
		ctx->fallback_tfm = crypto_alloc_sync_skcipher(name, 0,
							       CRYPTO_ALG_NEED_FALLBACK);
		if (IS_ERR(ctx->fallback_tfm)) {
			dev_err(ctx->spacc->dev,
				"Cannot allocate fallback for %s\n", name);
			return PTR_ERR(ctx->fallback_tfm);
		}
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

static int spacc_aes_setkey_locked(struct crypto_skcipher *tfm, const u8 *key,
				   unsigned int keylen)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct crypto_sync_skcipher *fb = ctx->fallback_tfm;
	int err;

	if (keylen != AES_KEYSIZE_128 && keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_256)
		return -EINVAL;

	if (!fb) {
		fb = crypto_alloc_sync_skcipher(crypto_tfm_alg_name(&tfm->base),
						0, CRYPTO_ALG_NEED_FALLBACK);
		if (IS_ERR(fb))
			return PTR_ERR(fb);
	}

	crypto_sync_skcipher_clear_flags(fb, CRYPTO_TFM_REQ_MASK);
	crypto_sync_skcipher_set_flags(fb,
				       crypto_skcipher_get_flags(tfm) &
				       CRYPTO_TFM_REQ_MASK);

	err = crypto_sync_skcipher_setkey(fb, key, keylen);
	if (err) {
		if (!ctx->fallback_tfm)
			crypto_free_sync_skcipher(fb);
		return err;
	}

	ctx->fallback_tfm = fb;
	memzero_explicit(ctx->key, sizeof(ctx->key));
	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int spacc_aes_setkey(struct crypto_skcipher *tfm, const u8 *key,
			    unsigned int keylen)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	int err;

	mutex_lock(&ctx->lock);
	err = spacc_aes_setkey_locked(tfm, key, keylen);
	mutex_unlock(&ctx->lock);

	return err;
}

/*
 * Under memory bus contention the engine corrupts the final write beats
 * of a command: beat-aligned zeros or garbage in the last 8-24 bytes, ~1%
 * of commands under concurrent load, any mode and size (LS1024A silicon
 * defect; see the project report for the full investigation).  A single
 * crypto thread is clean over thousands of operations.  Default off:
 * software is always correct.
 *
 * Runtime-switchable on purpose (0644): single-threaded batch jobs enable
 * the engine without reloading the module.  Readers use READ_ONCE.
 */
static bool spacc_use_hw;
module_param_named(hw, spacc_use_hw, bool, 0644);

static int spacc_aes_crypt(struct skcipher_request *req, bool is_encrypt)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	struct spacc_dev *spacc = ctx->spacc;
	unsigned int ivsize = crypto_skcipher_ivsize(tfm);
	int err;

	if (req->cryptlen == 0)
		return 0;

	if (!ctx->is_ctr && (req->cryptlen % AES_BLOCK_SIZE))
		return -EINVAL;

	/*
	 * Every mode reaches the engine only as a single chunk.  Requests that
	 * need a second command lose the last block of a chunk under concurrent
	 * load -- measured on both CBC and CTR, ~0.2% of requests -- and nothing
	 * in the API can detect it afterwards.
	 */
	if (!READ_ONCE(spacc_use_hw) ||
	    req->cryptlen < SPACC_MIN_HW_BYTES ||
	    req->cryptlen > SPACC_MAX_CHUNK_BYTES ||
	    sg_nents_for_len(req->src, req->cryptlen) > SPACC_MAX_SG_ENTS ||
	    (req->src != req->dst &&
	     sg_nents_for_len(req->dst, req->cryptlen) > SPACC_MAX_SG_ENTS))
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, is_encrypt, req->base.flags);

	INIT_LIST_HEAD(&rctx->node);
	rctx->req = req;
	rctx->is_encrypt = is_encrypt;
	rctx->cbc_enc_busy = false;
	rctx->offset = 0;
	rctx->inflight_cmds = 0;
	rctx->sw_tail_len = 0;
	rctx->err = 0;
	if (ivsize) {
		memcpy(rctx->iv, req->iv, ivsize);
		memcpy(rctx->final_iv, req->iv, ivsize);
	}

	spin_lock_bh(&spacc->q_lock);
	err = crypto_enqueue_request(&spacc->queue, &req->base);
	spin_unlock_bh(&spacc->q_lock);

	queue_work(spacc->wq, &spacc->work);

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
			.base.cra_alignmask	= 0,
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
			.base.cra_alignmask	= 0,
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
			.base.cra_alignmask	= 0,
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

static int spacc_probe(struct platform_device *pdev)
{
	struct spacc_dev *spacc;
	struct resource *res;
	unsigned long flags;
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

	spacc->rst = devm_reset_control_get_optional_shared(&pdev->dev, "spacc");
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
	spacc->cipher_pg_sz = SPACC_CIPHER_PG_SZ;

	spacc_id = readl(spacc->pdu_regs + SPACC_REG_ID);
	dev_info(&pdev->dev,
		 "LS1024A SPACC pipelined engine probed (version %u.%u)\n",
		 (spacc_id >> 4) & 0x0f, spacc_id & 0x0f);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
		goto err_reset_assert;
	}

	spacc->req_pool = dmam_pool_create(dev_name(&pdev->dev), &pdev->dev,
					   SPACC_MAX_DDT_ENTS * sizeof(struct spacc_ddt),
					   8, 0);
	if (!spacc->req_pool) {
		ret = -ENOMEM;
		goto err_reset_assert;
	}

	/*
	 * One worker owns the slot ring and the hardware contexts, so max_active
	 * is 1 by design: pipeline depth comes from the ring, not from more
	 * workers.
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
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", spacc->irq, ret);
		goto err_wq_destroy;
	}

	for (i = 0; i < ARRAY_SIZE(spacc_algs_tmpl); i++) {
		spacc->algs[i] = spacc_algs_tmpl[i];
		spacc->algs[i].spacc = spacc;
		ret = crypto_register_skcipher(&spacc->algs[i].alg);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register algorithm %s: %d\n",
				spacc->algs[i].alg.base.cra_name, ret);
			while (--i >= 0)
				crypto_unregister_skcipher(&spacc->algs[i].alg);
			goto err_irq_free;
		}
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
	int i;

	for (i = 0; i < ARRAY_SIZE(spacc->algs); i++)
		crypto_unregister_skcipher(&spacc->algs[i].alg);

	/*
	 * Mark teardown, then drain the worker BEFORE cancelling the timer: a
	 * running spacc_work_fn that read the flag just before it was set may
	 * still re-arm the watchdog, and only a drained worker makes the
	 * subsequent cancel final.
	 */
	spacc->shutting_down = true;
	smp_mb();
	drain_workqueue(spacc->wq);
	cancel_delayed_work_sync(&spacc->watchdog);
	destroy_workqueue(spacc->wq);

	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	devm_free_irq(&pdev->dev, spacc->irq, spacc);

	/* Key material survives cra_exit in the MMIO context pages: wipe. */
	for (i = 0; i < SPACC_RING_SIZE; i++)
		memset_io(spacc->cipher_ctx_base + (i * spacc->cipher_pg_sz),
			  0, spacc->cipher_pg_sz);

	if (spacc->rst)
		reset_control_assert(spacc->rst);

	if (spacc->clk)
		clk_disable_unprepare(spacc->clk);
}

static const struct of_device_id spacc_of_match[] = {
	{ .compatible = "fsl,ls1024a-spacc" },
	{ .compatible = "picochip,spacc-ls1024a" },
	{ .compatible = "picochip,spacc-ipsec" },
	{ .compatible = "picochip,spacc-l2" },
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

MODULE_DESCRIPTION("Freescale LS1024A SPACC Pipelined Crypto Engine Driver");
MODULE_AUTHOR("Antigravity Team");
MODULE_LICENSE("GPL");