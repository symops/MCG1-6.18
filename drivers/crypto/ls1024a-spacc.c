// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freescale LS1024A SPACC Crypto Engine Driver
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <crypto/aes.h>
#include <crypto/engine.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/skcipher.h>
#include <linux/atomic.h>
#include <linux/bottom_half.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
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

/* Engine limits and tuning */
#define SPACC_MAX_DDT_ENTS		64
#define SPACC_MAX_SG_ENTS		(SPACC_MAX_DDT_ENTS - 1)
#define SPACC_CIPHER_PG_SZ		128
#define SPACC_CIPHER_IV_OFFSET		32
#define SPACC_CRYPTO_BASE_OFFSET	0x00040000
#define SPACC_MIN_MMIO_SIZE		0x00048000

/*
 * Clamp chunk size to the largest AES-block-aligned value that fits in
 * 16 bits (65520 B), guaranteeing single-entry DDT and PROC_LEN safety
 * across all hardware revisions.
 */
#define SPACC_MAX_CHUNK_BYTES		round_down(SZ_64K - 1, AES_BLOCK_SIZE)

/*
 * Hardware setup and DMA round-trip overhead dominates for small requests.
 * Requests smaller than 8 KiB are therefore processed in software.
 */
#define SPACC_MIN_HW_BYTES		(8 * 1024)

/* DDT format matching hardware descriptor */
struct spacc_ddt {
	u32 p;
	u32 len;
};

struct spacc_dev;

struct spacc_alg {
	struct skcipher_engine_alg alg;
	struct spacc_dev *spacc;
	u32 ctrl_default;
	bool is_ctr;
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
	struct crypto_engine *ce;
	struct completion done;
	/*
	 * Protects STATUS FIFO ownership, completion state and
	 * timeout/IRQ recovery paths.
	 */
	spinlock_t lock;
	bool req_active;
	int req_err;
	/* Round-robin index into 32 hardware cipher contexts */
	atomic_t next_ctx;
	/* ecb(aes), cbc(aes) and ctr(aes) */
	struct spacc_alg algs[3];
};

struct spacc_ctx {
	struct spacc_dev *spacc;
	struct crypto_sync_skcipher *fallback_tfm;
	u32 ctrl_default;
	u8 key[AES_MAX_KEY_SIZE] __aligned(4);
	unsigned int keylen;
	bool is_ctr;
	/* Serializes lazy fallback allocation and rekeying */
	struct mutex lock;
};

/* DDT lifetime is local to spacc_hw_chunk() */
struct spacc_reqctx {
	bool is_encrypt;
};

static inline void spacc_memcpy_toio32(void __iomem *dst, const void *src,
				       unsigned int bytes)
{
	u32 __iomem *dst32 = dst;
	const u8 *src8 = src;
	unsigned int words = bytes / 4;
	unsigned int i;

	/* Context key/IV writes must always be 32-bit word aligned */
	WARN_ON_ONCE(bytes % 4);

	for (i = 0; i < words; i++) {
		u32 val = get_unaligned((const u32 *)(src8 + i * 4));

		writel(val, dst32 + i);
	}
}

static inline void spacc_write_proc_len(struct spacc_dev *spacc, u32 len)
{
	writel(0, spacc->crypto_regs + SPACC_REG_PRE_AAD_LEN);
	writel(0, spacc->crypto_regs + SPACC_REG_POST_AAD_LEN);
	writel(len, spacc->crypto_regs + SPACC_REG_PROC_LEN);
	writel(0, spacc->crypto_regs + SPACC_REG_ICV_LEN);
	writel(0, spacc->crypto_regs + SPACC_REG_ICV_OFFSET);
	writel(0, spacc->crypto_regs + SPACC_REG_IV_OFFSET);
	writel(0, spacc->crypto_regs + SPACC_REG_SW_ID);
	writel(0, spacc->crypto_regs + SPACC_REG_AUX_INFO);
}

static inline void spacc_write_ctrl(struct spacc_dev *spacc, u32 ctrl)
{
	writel(ctrl, spacc->crypto_regs + SPACC_REG_CTRL);
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

/*
 * Bytes from the start of @sg achievable within @max_ents scatterlist
 * nodes (zero-length nodes contribute nothing to the span but still
 * count toward the limit), up to @len. @out_nents receives the node
 * count of that prefix, so dma_map_sg() maps the whole covered range.
 */
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

	/* Use GFP_ATOMIC so pool exhaustion falls back immediately to software */
	ddt = dma_pool_alloc(spacc->req_pool, GFP_ATOMIC, ddt_phys);
	if (!ddt) {
		dma_unmap_sg(spacc->dev, sgl, nents, dir);
		return NULL;
	}

	for_each_sg(sgl, cur, mapped_ents, i) {
		unsigned int len = min_t(unsigned int, sg_dma_len(cur), nbytes);

		/* Zero-length entries act as DDT terminators in hardware */
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

	return ddt;
}

static irqreturn_t spacc_irq_handler(int irq, void *dev_id)
{
	struct spacc_dev *spacc = dev_id;
	unsigned long flags;
	u32 stat, cstat, status, code;

	spin_lock_irqsave(&spacc->lock, flags);

	stat = readl(spacc->pdu_regs + SPACC_REG_IRQ_STAT);
	if (!(stat & (SPA_IRQ_STAT_STAT_MASK | SPA_IRQ_STAT_CMD))) {
		spin_unlock_irqrestore(&spacc->lock, flags);
		return IRQ_NONE;
	}

	/* Clear interrupt: source first, then top-level aggregator */
	cstat = readl(spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	if (cstat)
		writel(cstat, spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(stat, spacc->pdu_regs + SPACC_REG_IRQ_STAT);

	/*
	 * A request is complete only when its status word is present in the
	 * STATUS FIFO. Spurious or command-FIFO interrupts with an empty FIFO
	 * do not indicate job completion: signaling completion early would
	 * allow the caller to unmap buffers and free the DDT while DMA is
	 * still active. Also ignore late completions already claimed by the
	 * timeout recovery path.
	 */
	if (!spacc->req_active ||
	    (readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) & SPA_FIFO_STAT_EMPTY)) {
		spin_unlock_irqrestore(&spacc->lock, flags);
		return IRQ_HANDLED;
	}

	/* Pop status from FIFO */
	writel(~0, spacc->crypto_regs + SPACC_REG_STAT_POP);
	status = readl(spacc->crypto_regs + SPACC_REG_STATUS);
	code = (status & SPA_STATUS_RES_CODE_MASK) >> SPA_STATUS_RES_CODE_OFFSET;
	if (code != SPA_STATUS_OK)
		dev_warn_ratelimited(spacc->dev,
				     "SPACC: request failed, STATUS=0x%08x (code %u)\n",
				     status, code);

	spacc->req_err = spacc_status_to_errno(code);
	spacc->req_active = false;
	complete(&spacc->done);

	spin_unlock_irqrestore(&spacc->lock, flags);
	return IRQ_HANDLED;
}

/* Compute @len bytes in software; @iv is updated like the engine path does. */
static int spacc_sw_crypt(struct spacc_ctx *ctx, struct scatterlist *src,
			  struct scatterlist *dst, unsigned int len, u8 *iv,
			  bool is_encrypt, u32 flags)
{
	int err;

	/*
	 * fallback_tfm is initialized during setkey() before any crypt call
	 * and is never reallocated afterwards, so reading it locklessly is safe.
	 * The check must run before SYNC_SKCIPHER_REQUEST_ON_STACK(), which
	 * dereferences the tfm via crypto_skcipher_reqsize().
	 */
	if (unlikely(!ctx->fallback_tfm)) {
		dev_dbg(ctx->spacc->dev, "SPACC: fallback tfm not initialized\n");
		return -EINVAL;
	}

	{
		SYNC_SKCIPHER_REQUEST_ON_STACK(subreq, ctx->fallback_tfm);

		skcipher_request_set_sync_tfm(subreq, ctx->fallback_tfm);
		/*
		 * Strip MAY_BACKLOG: subreq has no completion callback, so if
		 * the fallback driver ever queues a backlogged request (which
		 * it should not, being a sync_skcipher), nobody would wake the
		 * caller up. MAY_SLEEP is propagated so that a future non-sync
		 * fb would know whether it may allocate.
		 */
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

	lockdep_assert_held(&spacc->lock);

	/*
	 * Drain any stale STATUS FIFO entries inherited from bootloader, or
	 * left behind by a failed reset pulse.
	 */
	while (limit-- &&
	       !(readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) &
		 SPA_FIFO_STAT_EMPTY)) {
		writel(~0, spacc->crypto_regs + SPACC_REG_STAT_POP);
		(void)readl(spacc->crypto_regs + SPACC_REG_STATUS);
	}

	/* Clear any stale pending interrupts */
	writel(~0, spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	writel(~0, spacc->pdu_regs + SPACC_REG_IRQ_STAT);

	/*
	 * Set IRQ threshold to 1 and enable the status interrupt only: the
	 * command-FIFO interrupt carries no completion information.
	 */
	writel(1 << SPA_IRQ_CTRL_STAT_CNT_OFFSET,
	       spacc->crypto_regs + SPACC_REG_IRQ_CTRL);
	writel(SPA_IRQ_EN_STAT_EN | SPA_IRQ_EN_GLBL_EN,
	       spacc->pdu_regs + SPACC_REG_IRQ_EN);
	spacc->req_active = false;
}

/*
 * Three-phase hardware recovery after a timeout:
 *   1. Mask interrupts under spinlock.
 *   2. Wait for any active IRQ handler and pulse reset outside spinlock
 *      (reset_control_* may sleep).
 *   3. Drain stale STATUS FIFO words and re-enable interrupts under spinlock.
 */
static int spacc_hw_full_reset(struct spacc_dev *spacc)
{
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&spacc->lock, flags);
	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	spin_unlock_irqrestore(&spacc->lock, flags);

	synchronize_irq(spacc->irq);
	if (spacc->rst) {
		err = reset_control_assert(spacc->rst);
		if (!err) {
			fsleep(10);
			err = reset_control_deassert(spacc->rst);
		}
		if (err)
			dev_err(spacc->dev, "SPACC: reset control failed: %d\n",
				err);
	}

	/* Always restore IRQ mask and drain FIFO even if reset_control failed */
	spin_lock_irqsave(&spacc->lock, flags);
	spacc_hw_init_regs(spacc);
	spin_unlock_irqrestore(&spacc->lock, flags);

	return err;
}

static int spacc_hw_submit_and_wait(struct spacc_dev *spacc, u32 ctrl)
{
	unsigned long flags;
	unsigned long time_left;
	int err;

	spin_lock_irqsave(&spacc->lock, flags);
	reinit_completion(&spacc->done);
	spacc->req_err = 0;
	spacc->req_active = true;

	/* Start operation by writing CTRL */
	spacc_write_ctrl(spacc, ctrl);
	spin_unlock_irqrestore(&spacc->lock, flags);

	time_left = wait_for_completion_timeout(&spacc->done,
						msecs_to_jiffies(1000));

	/*
	 * Serialize FIFO polling against spacc_irq_handler() so a late IRQ
	 * racing with the timeout boundary cannot double-pop STATUS FIFO.
	 */
	spin_lock_irqsave(&spacc->lock, flags);
	if (!spacc->req_active) {
		err = spacc->req_err;
	} else if (!(readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) &
		     SPA_FIFO_STAT_EMPTY)) {
		u32 status, code;

		writel(~0, spacc->crypto_regs + SPACC_REG_STAT_POP);
		status = readl(spacc->crypto_regs + SPACC_REG_STATUS);
		code = (status & SPA_STATUS_RES_CODE_MASK) >>
		       SPA_STATUS_RES_CODE_OFFSET;
		if (!time_left)
			dev_warn_ratelimited(spacc->dev,
					     "SPACC: completed in FIFO without IRQ (STATUS=0x%08x)\n",
					     status);
		err = spacc_status_to_errno(code);
		spacc->req_active = false;
	} else {
		spacc->req_active = false;
		err = -ETIMEDOUT;
	}
	spin_unlock_irqrestore(&spacc->lock, flags);

	if (err == -ETIMEDOUT) {
		int reset_err;

		dev_err_ratelimited(spacc->dev,
				    "SPACC: request timed out (IRQ_STAT=0x%08x FIFO_STAT=0x%08x), resetting engine\n",
				    readl(spacc->crypto_regs + SPACC_REG_IRQ_STAT),
				    readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT));
		reset_err = spacc_hw_full_reset(spacc);
		if (reset_err) {
			dev_err(spacc->dev, "SPACC: hardware reset failed: %d\n",
				reset_err);
			return reset_err;
		}
	}

	return err;
}

/*
 * Program the engine for one contiguous chunk and wait for its completion.
 * crypto_engine processes requests sequentially, therefore the register file
 * is owned exclusively by the currently executing request.
 */
static int spacc_hw_chunk(struct spacc_dev *spacc, struct spacc_ctx *ctx,
			  struct scatterlist *src, struct scatterlist *dst,
			  unsigned int len, unsigned int src_nents,
			  unsigned int dst_nents, unsigned int ivsize,
			  const u8 *iv, bool encrypt)
{
	struct spacc_ddt *src_ddt, *dst_ddt;
	dma_addr_t src_addr, dst_addr;
	unsigned int ctx_id;
	void __iomem *page;
	u32 ctrl;
	int err;

	/*
	 * PROC_LEN is 16-bit; spacc_do_one_request() clamps to
	 * SPACC_MAX_CHUNK_BYTES, this is a backstop for any future caller.
	 */
	if (WARN_ON_ONCE(len > 0xFFFF))
		return -EINVAL;

	if (src == dst) {
		src_ddt = spacc_sg_to_ddt(spacc, src, len, src_nents,
					  DMA_BIDIRECTIONAL, &src_addr);
		if (!src_ddt)
			return -ENOMEM;
		dst_ddt = src_ddt;
		dst_addr = src_addr;
	} else {
		src_ddt = spacc_sg_to_ddt(spacc, src, len, src_nents,
					  DMA_TO_DEVICE, &src_addr);
		if (!src_ddt)
			return -ENOMEM;
		dst_ddt = spacc_sg_to_ddt(spacc, dst, len, dst_nents,
					  DMA_FROM_DEVICE, &dst_addr);
		if (!dst_ddt) {
			dma_unmap_sg(spacc->dev, src, src_nents, DMA_TO_DEVICE);
			dma_pool_free(spacc->req_pool, src_ddt, src_addr);
			return -ENOMEM;
		}
	}

	ctx_id = (atomic_inc_return(&spacc->next_ctx) - 1) & 0x1f;
	page = spacc->cipher_ctx_base + (ctx_id * spacc->cipher_pg_sz);

	spacc_memcpy_toio32(page, ctx->key, ctx->keylen);
	if (ivsize)
		spacc_memcpy_toio32(page + SPACC_CIPHER_IV_OFFSET, iv, ivsize);

	writel(ctx->keylen | (ctx_id << SPA_KEY_SZ_CTX_INDEX_OFFSET) |
	       (1U << SPA_KEY_SZ_CIPHER_OFFSET),
	       spacc->crypto_regs + SPACC_REG_KEY_SZ);

	writel(src_addr, spacc->crypto_regs + SPACC_REG_SRC_PTR);
	writel(dst_addr, spacc->crypto_regs + SPACC_REG_DST_PTR);
	writel(0, spacc->crypto_regs + SPACC_REG_OFFSET);

	spacc_write_proc_len(spacc, len);

	ctrl = ctx->ctrl_default | (ctx_id << SPA_CTRL_CTX_IDX);
	if (encrypt) {
		ctrl |= BIT(SPA_CTRL_ENCRYPT_IDX);
	} else {
		/*
		 * For AES decryption the context holds the raw cipher key
		 * rather than a pre-expanded last-round key, so instruct the
		 * engine to expand the decryption key schedule itself.
		 */
		ctrl |= BIT(SPA_CTRL_KEY_EXP);
	}

	err = spacc_hw_submit_and_wait(spacc, ctrl);

	if (src == dst) {
		dma_unmap_sg(spacc->dev, src, src_nents, DMA_BIDIRECTIONAL);
		dma_pool_free(spacc->req_pool, src_ddt, src_addr);
	} else {
		dma_unmap_sg(spacc->dev, src, src_nents, DMA_TO_DEVICE);
		dma_unmap_sg(spacc->dev, dst, dst_nents, DMA_FROM_DEVICE);
		dma_pool_free(spacc->req_pool, src_ddt, src_addr);
		dma_pool_free(spacc->req_pool, dst_ddt, dst_addr);
	}

	return err;
}

/*
 * Advance a 16-byte big-endian counter by the blocks one chunk consumed.  The
 * engine takes the counter block as-is, so a multi-chunk CTR request has to
 * carry it forward itself.
 */
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

static int spacc_do_one_request(struct crypto_engine *ce, void *areq)
{
	struct skcipher_request *req = skcipher_request_cast(areq);
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);
	struct spacc_dev *spacc = ctx->spacc;
	unsigned int ivsize = crypto_skcipher_ivsize(tfm);
	bool in_place = (req->src == req->dst);
	unsigned int done = 0;
	u8 iv[AES_BLOCK_SIZE] __aligned(4);
	u8 next_iv[AES_BLOCK_SIZE] __aligned(4) = { 0 };
	int err = 0;

	if (req->cryptlen == 0)
		goto finalize;

	if (ivsize)
		memcpy(iv, req->iv, ivsize);

	/*
	 * One engine command takes one DDT per direction: SPACC_MAX_DDT_ENTS - 1
	 * scatterlist entries plus the terminator.  Cut the request into pieces
	 * that fit, by entry count as well as by size, and chain the CBC IV
	 * between pieces.  What the engine cannot take is computed in software.
	 */
	while (done < req->cryptlen) {
		struct scatterlist src_chain[2], dst_chain[2];
		struct scatterlist *src, *dst;
		unsigned int left = req->cryptlen - done;
		unsigned int chunk;
		unsigned int src_nents = 0, dst_nents = 0;

		src = scatterwalk_ffwd(src_chain, req->src, done);
		dst = in_place ? src : scatterwalk_ffwd(dst_chain, req->dst, done);

		chunk = min_t(unsigned int, left, SPACC_MAX_CHUNK_BYTES);
		/*
		 * The engine carries only the low 32 bits of the counter and wraps
		 * them to zero without touching byte 11.  End the chunk at that
		 * boundary; spacc_ctr_advance() then continues the 128-bit count.
		 */
		if (ctx->is_ctr) {
			u32 low = ((u32)iv[12] << 24) | ((u32)iv[13] << 16) |
				  ((u32)iv[14] << 8) | iv[15];
			u64 limit = (0x100000000ULL - low) * AES_BLOCK_SIZE;
			if (limit < chunk)
				chunk = (unsigned int)limit;
		}

		chunk = spacc_sg_span(src, chunk, SPACC_MAX_SG_ENTS, &src_nents);
		if (!in_place)
			chunk = spacc_sg_span(dst, chunk, SPACC_MAX_SG_ENTS,
					      &dst_nents);
		else
			dst_nents = src_nents;
		chunk = round_down(chunk, AES_BLOCK_SIZE);

		/*
		 * Less than an engine round trip is worth -- a short tail, or a
		 * list so fragmented that one DDT covers no more: finish in
		 * software.  (Whole requests this small never get here.)
		 */
		if (chunk < SPACC_MIN_HW_BYTES) {
			err = -ENOMEM;
		} else {
			/*
			 * For decryption the next IV is the last ciphertext block
			 * of this chunk.  With src == dst the engine overwrites it
			 * in place, so save it before the chunk is started.
			 */
			if (!ctx->is_ctr && !rctx->is_encrypt && ivsize)
				scatterwalk_map_and_copy(next_iv, src,
							 chunk - ivsize, ivsize, 0);

			err = spacc_hw_chunk(spacc, ctx, src, dst, chunk,
					     src_nents, dst_nents, ivsize, iv,
					     rctx->is_encrypt);
		}

		/*
		 * -ENOMEM: nothing was started (too small, or no mapping/DDT).
		 * iv already points at this position -- every completed chunk
		 * advanced it -- so the software part continues from here.
		 */
		if (err == -ENOMEM) {
			err = spacc_sw_crypt(ctx, src, dst, left, iv,
					     rctx->is_encrypt, req->base.flags);
			break;
		}
		if (err)
			break;

		done += chunk;

		/*
		 * ctr(aes) increments the request IV for every counter block it
		 * consumes, the last one included.  Match that for the hardware
		 * path as well, so the IV left behind does not depend on which
		 * path the request took.
		 */
		if (ctx->is_ctr) {
			spacc_ctr_advance(iv, chunk);
		} else if (ivsize) {
			/*
			 * CBC leaves the last ciphertext block in the request IV,
			 * the last chunk included: a caller may split the data and
			 * feed that IV into the next request.
			 */
			if (rctx->is_encrypt)
				scatterwalk_map_and_copy(iv, dst, chunk - ivsize,
							 ivsize, 0);
			else
				memcpy(iv, next_iv, ivsize);
		}
	}

	if (!err && ivsize)
		memcpy(req->iv, iv, ivsize);

finalize:
	local_bh_disable();
	crypto_finalize_skcipher_request(ce, req, err);
	local_bh_enable();
	return 0;
}

static int spacc_cra_init(struct crypto_skcipher *tfm)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct spacc_alg *spacc_alg = container_of(alg, struct spacc_alg, alg.base);
	const char *name = crypto_tfm_alg_name(&tfm->base);

	ctx->spacc = spacc_alg->spacc;
	ctx->ctrl_default = spacc_alg->ctrl_default;
	ctx->is_ctr = spacc_alg->is_ctr;
	mutex_init(&ctx->lock);

	/*
	 * CTR has no software fallback: allocating one here would ask the crypto
	 * manager for a "ctr(aes)" implementation while it is busy instantiating
	 * this very algorithm as a child of gcm(aes), which fails with -EAGAIN
	 * and gets the algorithm blacklisted.  The engine handles CTR directly.
	 */
	if (!ctx->is_ctr) {
		ctx->fallback_tfm = crypto_alloc_sync_skcipher(name, 0,
							       CRYPTO_ALG_NEED_FALLBACK);
		if (IS_ERR(ctx->fallback_tfm)) {
			dev_err(ctx->spacc->dev,
				"Cannot allocate fallback for %s\n", name);
			return PTR_ERR(ctx->fallback_tfm);
		}

		dev_info_once(ctx->spacc->dev,
			      "SPACC: software path for %s is %s\n", name,
			      crypto_tfm_alg_driver_name(
				      crypto_skcipher_tfm(&ctx->fallback_tfm->base)));
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

	/*
	 * CTR has no fallback at init time (see cra_init); allocate it here,
	 * where the crypto manager is free, so that small requests can still be
	 * computed in software.
	 */
	if (!fb) {
		fb = crypto_alloc_sync_skcipher(crypto_tfm_alg_name(&tfm->base),
						0, CRYPTO_ALG_NEED_FALLBACK);
		if (IS_ERR(fb)) {
			err = PTR_ERR(fb);
			dev_err(ctx->spacc->dev,
				"Cannot allocate fallback for %s: %d\n",
				crypto_tfm_alg_name(&tfm->base), err);
			return err;
		}
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
	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int spacc_aes_setkey(struct crypto_skcipher *tfm, const u8 *key,
			    unsigned int keylen)
{
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	int err;

	/*
	 * Callers may rekey concurrently; the key, the fallback key and the
	 * lazy fallback allocation are all shared state of this transform.
	 */
	mutex_lock(&ctx->lock);
	err = spacc_aes_setkey_locked(tfm, key, keylen);
	mutex_unlock(&ctx->lock);

	return err;
}

static int spacc_aes_crypt(struct skcipher_request *req, bool is_encrypt)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_reqctx *rctx = skcipher_request_ctx(req);

	if (req->cryptlen == 0)
		return 0;

	/*
	 * ECB and CBC operate strictly on full AES blocks. CTR is a stream
	 * mode with blocksize 1: small non-aligned requests (< SPACC_MIN_HW_BYTES)
	 * take the synchronous software path below, while larger ones run
	 * their block-aligned prefix on SPACC and finish the trailing
	 * sub-block tail in spacc_do_one_request().
	 */
	if (!ctx->is_ctr && (req->cryptlen % AES_BLOCK_SIZE))
		return -EINVAL;

	/*
	 * Synchronous software path: the result is the return value.  The
	 * completion callback must not be called as well -- it belongs to
	 * requests that returned -EINPROGRESS, and a stray call leaves a
	 * completion pending in the caller's crypto_wait, which makes its next
	 * asynchronous request "finish" before the engine has done it.
	 */
	if (req->cryptlen < SPACC_MIN_HW_BYTES)
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, is_encrypt, req->base.flags);

	rctx->is_encrypt = is_encrypt;

	return crypto_transfer_skcipher_request_to_engine(ctx->spacc->ce, req);
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
	/* CTR decrypts by encrypting: the keystream is the same either way. */
	return spacc_aes_crypt(req, true);
}

static const struct spacc_alg spacc_algs_tmpl[] = {
	{
		.ctrl_default = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_CTR,
		.is_ctr = true,
		.alg = {
			.base = {
				.base.cra_name		= "ctr(aes)",
				.base.cra_driver_name	= "ctr-aes-ls1024a-spacc",
				.base.cra_priority	= 300,
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
			.op = {
				.do_one_request = spacc_do_one_request,
			},
		},
	},
	{
		.ctrl_default = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_ECB,
		.alg = {
			.base = {
				.base.cra_name		= "ecb(aes)",
				.base.cra_driver_name	= "ecb-aes-ls1024a-spacc",
				.base.cra_priority	= 300,
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
			.op = {
				.do_one_request = spacc_do_one_request,
			},
		},
	},
	{
		.ctrl_default = SPA_CTRL_CIPH_ALG_AES | SPA_CTRL_CIPH_MODE_CBC,
		.alg = {
			.base = {
				.base.cra_name		= "cbc(aes)",
				.base.cra_driver_name	= "cbc-aes-ls1024a-spacc",
				.base.cra_priority	= 300,
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
			.op = {
				.do_one_request = spacc_do_one_request,
			},
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
	init_completion(&spacc->done);
	spin_lock_init(&spacc->lock);
	atomic_set(&spacc->next_ctx, 0);

	/* Clocks & Resets */
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

	/* Map MMIO registers */
	spacc->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(spacc->regs)) {
		ret = PTR_ERR(spacc->regs);
		goto err_reset_assert;
	}

	if (resource_size(res) < SPACC_MIN_MMIO_SIZE) {
		ret = -EINVAL;
		goto err_reset_assert;
	}

	spacc->pdu_regs = spacc->regs;
	spacc->crypto_regs = spacc->regs + SPACC_CRYPTO_BASE_OFFSET;
	spacc->cipher_ctx_base = spacc->crypto_regs + SPACC_REG_CIPH_KEY_BASE;
	spacc->cipher_pg_sz = SPACC_CIPHER_PG_SZ;

	spacc_id = readl(spacc->pdu_regs + SPACC_REG_ID);
	dev_info(&pdev->dev, "LS1024A SPACC engine probed (version %u.%u)\n",
		 (spacc_id >> 4) & 0x0f, spacc_id & 0x0f);

	/* Set DMA mask */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
		goto err_reset_assert;
	}

	/* Allocate DMA pool for DDT descriptors */
	spacc->req_pool = dmam_pool_create(dev_name(&pdev->dev), &pdev->dev,
					   SPACC_MAX_DDT_ENTS * sizeof(struct spacc_ddt),
					   8, 0);
	if (!spacc->req_pool) {
		ret = -ENOMEM;
		goto err_reset_assert;
	}

	/* Interrupt setup */
	spacc->irq = platform_get_irq(pdev, 0);
	if (spacc->irq < 0) {
		ret = spacc->irq;
		goto err_reset_assert;
	}

	spin_lock_irqsave(&spacc->lock, flags);
	spacc_hw_init_regs(spacc);
	spin_unlock_irqrestore(&spacc->lock, flags);

	ret = devm_request_irq(&pdev->dev, spacc->irq, spacc_irq_handler, 0,
			       dev_name(&pdev->dev), spacc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", spacc->irq, ret);
		goto err_reset_assert;
	}

	/* Initialize crypto engine without real-time thread priority */
	spacc->ce = crypto_engine_alloc_init(&pdev->dev, false);
	if (!spacc->ce) {
		ret = -ENOMEM;
		goto err_irq_free;
	}

	ret = crypto_engine_start(spacc->ce);
	if (ret)
		goto err_ce_exit;

	/* Register algorithms */
	for (i = 0; i < ARRAY_SIZE(spacc_algs_tmpl); i++) {
		spacc->algs[i] = spacc_algs_tmpl[i];
		spacc->algs[i].spacc = spacc;
		ret = crypto_engine_register_skcipher(&spacc->algs[i].alg);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register algorithm %s: %d\n",
				spacc->algs[i].alg.base.base.cra_name, ret);
			while (--i >= 0)
				crypto_engine_unregister_skcipher(&spacc->algs[i].alg);
			goto err_ce_exit;
		}
	}

	platform_set_drvdata(pdev, spacc);

	return 0;

err_ce_exit:
	/* crypto_engine_exit() stops and destroys the worker internally */
	crypto_engine_exit(spacc->ce);
err_irq_free:
	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	devm_free_irq(&pdev->dev, spacc->irq, spacc);
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
		crypto_engine_unregister_skcipher(&spacc->algs[i].alg);

	/* crypto_engine_exit() stops and destroys the worker internally */
	crypto_engine_exit(spacc->ce);

	/* Mask, sync and detach IRQ before gating clocks to avoid unclocked MMIO */
	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
	devm_free_irq(&pdev->dev, spacc->irq, spacc);

	WARN_ON_ONCE(spacc->req_active);

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

MODULE_DESCRIPTION("Freescale LS1024A SPACC Crypto Engine Driver");
MODULE_AUTHOR("Antigravity Team");
MODULE_LICENSE("GPL");