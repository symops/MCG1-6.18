// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freescale LS1024A / Mindspeed Comcerto SPACC Crypto Engine Driver
 *
 * Copyright (C) 2026 Antigravity Project
 * Based on Elliptic / Picochip SPACC architecture.
 */

#include <crypto/aes.h>
#include <crypto/engine.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/skcipher.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>

#define DRV_NAME			"ls1024a-spacc"

/* Hardware Register Offsets */
#define SPACC_REG_IRQ_EN		0x00000000
#define SPACC_REG_IRQ_STAT		0x00000004
#define SPACC_REG_IRQ_CTRL		0x00000008
#define SPACC_REG_FIFO_STAT		0x0000000C
#define SPACC_REG_SDMA_BRST_SZ		0x00000010
#define SPACC_REG_SRC_PTR		0x00000020
#define SPACC_REG_DST_PTR		0x00000024
#define SPACC_REG_OFFSET		0x00000028

/* SPACC 2.0 specific offsets */
#define SPACC2_REG_AAD_LEN		0x0000002C
#define SPACC2_REG_PROC_LEN		0x00000030
#define SPACC2_REG_ICV_LEN		0x00000034
#define SPACC2_REG_ICV_OFFSET		0x00000038
#define SPACC2_REG_SW_CTRL		0x0000003C
#define SPACC2_REG_CTRL			0x00000040
#define SPACC2_REG_AUX_INFO		0x0000004C

/* SPACC 3.0 specific offsets */
#define SPACC3_REG_PRE_AAD_LEN		0x0000002C
#define SPACC3_REG_POST_AAD_LEN		0x00000030
#define SPACC3_REG_PROC_LEN		0x00000034
#define SPACC3_REG_ICV_LEN		0x00000038
#define SPACC3_REG_ICV_OFFSET		0x0000003C
#define SPACC3_REG_IV_OFFSET		0x00000040
#define SPACC3_REG_SW_ID		0x00000044
#define SPACC3_REG_AUX_INFO		0x00000048
#define SPACC3_REG_CTRL			0x0000004C

/* Common offsets */
#define SPACC_REG_STAT_POP		0x00000050
#define SPACC_REG_STATUS		0x00000054
#define SPACC_REG_KEY_SZ		0x00000100
#define SPACC_REG_ID			0x00000180
#define SPACC_REG_CIPH_KEY_BASE		0x00004000
#define SPACC_REG_HASH_KEY_BASE		0x00008000

/* Status codes */
#define SPA_STATUS_OK			0
#define SPA_STATUS_ICV_FAIL		1
#define SPA_STATUS_MEMORY_ERROR		2
#define SPA_STATUS_BLOCK_ERROR		3

#define SPA_STATUS_RES_CODE_OFFSET	24
#define SPA_STATUS_RES_CODE_MASK	(0x3 << SPA_STATUS_RES_CODE_OFFSET)

/* IRQ control/status bits */
#define SPA_IRQ_EN_CMD0_EN		BIT(0)
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

#define SPACC_MAX_DDT_ENTS		64
#define SPACC_CIPHER_PG_SZ		128
#define SPACC_CRYPTO_BASE_OFFSET	0x00040000

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
	bool ddt_logged;
	bool timing_logged;
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
	int req_err;
	unsigned int next_ctx;
	bool is_spacc3;
	/* ecb(aes), cbc(aes) and ctr(aes) */
	struct spacc_alg algs[3];
};

struct spacc_ctx {
	struct spacc_dev *spacc;
	struct crypto_sync_skcipher *fallback_tfm;
	u32 ctrl_default;
	u8 key[AES_MAX_KEY_SIZE] __aligned(4);
	unsigned int keylen;
	bool use_fallback;
	bool is_ctr;
	/* serializes lazy fallback allocation and rekeying */
	struct mutex lock;
};

struct spacc_reqctx {
	bool is_encrypt;
	dma_addr_t src_addr;
	dma_addr_t dst_addr;
	int src_nents;
	int dst_nents;
	struct spacc_ddt *src_ddt;
	struct spacc_ddt *dst_ddt;
	u8 backup_iv[AES_BLOCK_SIZE];
};

static inline void spacc_memcpy_toio32(void __iomem *dst, const void *src, unsigned int bytes)
{
	u32 __iomem *dst32 = dst;
	unsigned int words = bytes / 4;
	unsigned int i;

	for (i = 0; i < words; i++) {
		u32 val = get_unaligned((const u32 *)src + i);

		writel(val, dst32 + i);
	}
}

static inline void spacc_write_proc_len(struct spacc_dev *spacc, u32 len)
{
	if (spacc->is_spacc3) {
		writel(0, spacc->crypto_regs + SPACC3_REG_PRE_AAD_LEN);
		writel(0, spacc->crypto_regs + SPACC3_REG_POST_AAD_LEN);
		writel(len, spacc->crypto_regs + SPACC3_REG_PROC_LEN);
		writel(0, spacc->crypto_regs + SPACC3_REG_ICV_LEN);
		writel(0, spacc->crypto_regs + SPACC3_REG_ICV_OFFSET);
		writel(0, spacc->crypto_regs + SPACC3_REG_IV_OFFSET);
		writel(0, spacc->crypto_regs + SPACC3_REG_SW_ID);
		writel(0, spacc->crypto_regs + SPACC3_REG_AUX_INFO);
	} else {
		writel(0, spacc->crypto_regs + SPACC2_REG_AAD_LEN);
		writel(len, spacc->crypto_regs + SPACC2_REG_PROC_LEN);
		writel(0, spacc->crypto_regs + SPACC2_REG_ICV_LEN);
		writel(0, spacc->crypto_regs + SPACC2_REG_ICV_OFFSET);
		writel(0, spacc->crypto_regs + SPACC2_REG_SW_CTRL);
		writel(0, spacc->crypto_regs + SPACC2_REG_AUX_INFO);
	}
}

static inline void spacc_write_ctrl(struct spacc_dev *spacc, u32 ctrl)
{
	if (spacc->is_spacc3)
		writel(ctrl, spacc->crypto_regs + SPACC3_REG_CTRL);
	else
		writel(ctrl, spacc->crypto_regs + SPACC2_REG_CTRL);
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
	case SPA_STATUS_BLOCK_ERROR:
		return -EIO;
	default:
		return -EINVAL;
	}
}

static struct spacc_ddt *spacc_sg_to_ddt(struct spacc_dev *spacc,
					 struct scatterlist *sgl,
					 unsigned int nbytes,
					 enum dma_data_direction dir,
					 dma_addr_t *ddt_phys,
					 int *out_nents)
{
	struct spacc_ddt *ddt;
	struct scatterlist *cur;
	int nents, mapped_ents, i;

	nents = sg_nents_for_len(sgl, nbytes);
	if (nents < 0)
		return NULL;

	mapped_ents = dma_map_sg(spacc->dev, sgl, nents, dir);
	if (mapped_ents <= 0)
		return NULL;

	if (mapped_ents + 1 > SPACC_MAX_DDT_ENTS) {
		dma_unmap_sg(spacc->dev, sgl, nents, dir);
		return NULL;
	}

	ddt = dma_pool_alloc(spacc->req_pool, GFP_ATOMIC, ddt_phys);
	if (!ddt) {
		dma_unmap_sg(spacc->dev, sgl, nents, dir);
		return NULL;
	}

	for_each_sg(sgl, cur, mapped_ents, i) {
		unsigned int len = min_t(unsigned int, sg_dma_len(cur), nbytes);

		ddt[i].p = sg_dma_address(cur);
		ddt[i].len = len;
		nbytes -= len;
		if (nbytes == 0) {
			i++;
			break;
		}
	}
	ddt[i].p = 0;
	ddt[i].len = 0;

	*out_nents = nents;
	return ddt;
}

static irqreturn_t spacc_irq_handler(int irq, void *dev_id)
{
	struct spacc_dev *spacc = dev_id;
	u32 stat = readl(spacc->pdu_regs + SPACC_REG_IRQ_STAT);
	u32 status, code;

	if (!(stat & (SPA_IRQ_STAT_STAT_MASK | SPA_IRQ_STAT_CMD)))
		return IRQ_NONE;

	/* Clear interrupt */
	writel(stat, spacc->pdu_regs + SPACC_REG_IRQ_STAT);
	if (spacc->crypto_regs != spacc->pdu_regs) {
		u32 cstat = readl(spacc->crypto_regs + SPACC_REG_IRQ_STAT);

		if (cstat)
			writel(cstat, spacc->crypto_regs + SPACC_REG_IRQ_STAT);
	}

	/*
	 * A request is finished only when its status word is in the STATUS FIFO.
	 * An interrupt with the FIFO still empty (e.g. the command-FIFO one) says
	 * nothing about completion: reporting success here let the caller unmap
	 * the buffers and free/reuse the DDT while the engine was still running,
	 * and left the real status to be taken for the next request's.
	 */
	if (readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) & SPA_FIFO_STAT_EMPTY)
		return IRQ_HANDLED;

	/* Pop status from FIFO */
	writel(~0, spacc->crypto_regs + SPACC_REG_STAT_POP);
	status = readl(spacc->crypto_regs + SPACC_REG_STATUS);
	code = (status & SPA_STATUS_RES_CODE_MASK) >> SPA_STATUS_RES_CODE_OFFSET;
	if (code != SPA_STATUS_OK)
		dev_warn_ratelimited(spacc->dev,
				     "SPACC: request failed, STATUS=0x%08x (code %u)\n",
				     status, code);

	spacc->req_err = spacc_status_to_errno(code);
	complete(&spacc->done);

	return IRQ_HANDLED;
}

/*
 * The engine costs a fixed ~165 us per command: MMIO programming, DMA mapping
 * and the completion round trip.  Below roughly 8 KiB that dominates the AES
 * work itself, so small requests are cheaper to compute in software.  Measured
 * on this SoC through AF_ALG (Documentation/arm/ls1024a-spacc-bench.sh,
 * AES-128-CBC, engine vs OpenSSL's software AES): 8 KiB -> 25.0 vs 22.9 MB/s,
 * 16 KiB -> 34.2 vs 22.9, 64 KiB -> 47.8 vs 22.2, 1 MiB -> 54.5 vs 22.4.  The
 * engine side is the same with a 256-bit key; software drops to ~17 MB/s.
 * An earlier run with the engine forced at 4 KiB gave 5.9 MB/s.
 */
#define SPACC_MIN_HW_BYTES	(8 * 1024)

/* Compute @len bytes in software; @iv is updated like the engine path does. */
static int spacc_sw_crypt(struct spacc_ctx *ctx, struct scatterlist *src,
			  struct scatterlist *dst, unsigned int len, u8 *iv,
			  bool is_encrypt, u32 flags)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(subreq, ctx->fallback_tfm);
	int err;

	skcipher_request_set_sync_tfm(subreq, ctx->fallback_tfm);
	skcipher_request_set_callback(subreq, flags, NULL, NULL);
	skcipher_request_set_crypt(subreq, src, dst, len, iv);

	if (is_encrypt)
		err = crypto_skcipher_encrypt(subreq);
	else
		err = crypto_skcipher_decrypt(subreq);

	skcipher_request_zero(subreq);
	return err;
}

/* Bytes from the start of @sg that @max_ents DDT entries can describe, up to @len. */
static unsigned int spacc_sg_span(struct scatterlist *sg, unsigned int len,
				  unsigned int max_ents)
{
	unsigned int span = 0;

	for (; sg && max_ents && span < len; sg = sg_next(sg), max_ents--)
		span += min(sg->length, len - span);

	return span;
}

#define SPACC_MAX_CHUNK_BYTES	(128 * 1024)

/* Program the engine for one contiguous chunk and wait for its completion. */
static DEFINE_MUTEX(spacc_hw_lock);

static int spacc_hw_chunk_locked(struct spacc_dev *spacc, struct spacc_ctx *ctx,
			  struct scatterlist *src, struct scatterlist *dst,
			  unsigned int len, unsigned int ivsize,
			  const u8 *iv, bool encrypt)
{
	struct spacc_ddt *src_ddt, *dst_ddt;
	dma_addr_t src_addr, dst_addr;
	int src_nents = 0, dst_nents = 0;
	unsigned int ctx_id;
	void __iomem *page;
	long time_left;
	u32 ctrl;
	int err;

	if (src == dst) {
		src_ddt = spacc_sg_to_ddt(spacc, src, len, DMA_BIDIRECTIONAL,
					  &src_addr, &src_nents);
		if (!src_ddt)
			return -ENOMEM;
		dst_ddt = src_ddt;
		dst_addr = src_addr;
		dst_nents = src_nents;
	} else {
		src_ddt = spacc_sg_to_ddt(spacc, src, len, DMA_TO_DEVICE,
					  &src_addr, &src_nents);
		if (!src_ddt)
			return -ENOMEM;
		dst_ddt = spacc_sg_to_ddt(spacc, dst, len, DMA_FROM_DEVICE,
					  &dst_addr, &dst_nents);
		if (!dst_ddt) {
			dma_unmap_sg(spacc->dev, src, src_nents, DMA_TO_DEVICE);
			dma_pool_free(spacc->req_pool, src_ddt, src_addr);
			return -ENOMEM;
		}
	}

	ctx_id = (spacc->next_ctx++) & 0x1f;
	page = spacc->cipher_ctx_base + (ctx_id * spacc->cipher_pg_sz);

	spacc_memcpy_toio32(page, ctx->key, ctx->keylen);
	if (ivsize)
		spacc_memcpy_toio32(page + 32, iv, ivsize);

	writel(ctx->keylen | (ctx_id << SPA_KEY_SZ_CTX_INDEX_OFFSET) |
	       (1U << SPA_KEY_SZ_CIPHER_OFFSET),
	       spacc->crypto_regs + SPACC_REG_KEY_SZ);

	writel(src_addr, spacc->crypto_regs + SPACC_REG_SRC_PTR);
	writel(dst_addr, spacc->crypto_regs + SPACC_REG_DST_PTR);
	writel(0, spacc->crypto_regs + SPACC_REG_OFFSET);

	spacc_write_proc_len(spacc, len);

	ctrl = ctx->ctrl_default | (ctx_id << SPA_CTRL_CTX_IDX);
	if (encrypt)
		ctrl |= (1U << SPA_CTRL_ENCRYPT_IDX);
	else
		ctrl |= (1U << SPA_CTRL_KEY_EXP);

	reinit_completion(&spacc->done);

	/* Start operation by writing CTRL */
	spacc_write_ctrl(spacc, ctrl);

	time_left = wait_for_completion_timeout(&spacc->done, msecs_to_jiffies(1000));
	if (!time_left) {
		dev_warn_once(spacc->dev,
			      "SPACC: no IRQ within 1s (IRQ_STAT=0x%08x FIFO_STAT=0x%08x)\n",
			      readl(spacc->crypto_regs + SPACC_REG_IRQ_STAT),
			      readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT));
		if (!(readl(spacc->crypto_regs + SPACC_REG_FIFO_STAT) & SPA_FIFO_STAT_EMPTY)) {
			u32 status;

			writel(~0, spacc->crypto_regs + SPACC_REG_STAT_POP);
			status = readl(spacc->crypto_regs + SPACC_REG_STATUS);
			err = spacc_status_to_errno((status & SPA_STATUS_RES_CODE_MASK) >>
						     SPA_STATUS_RES_CODE_OFFSET);
		} else {
			dev_err(spacc->dev, "SPACC request timed out\n");
			err = -ETIMEDOUT;
		}
	} else {
		err = spacc->req_err;
	}

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

/* One set of registers, one command at a time. */
static int spacc_hw_chunk(struct spacc_dev *spacc, struct spacc_ctx *ctx,
			  struct scatterlist *src, struct scatterlist *dst,
			  unsigned int len, unsigned int ivsize,
			  const u8 *iv, bool encrypt)
{
	int err;

	mutex_lock(&spacc_hw_lock);
	err = spacc_hw_chunk_locked(spacc, ctx, src, dst, len, ivsize, iv,
				    encrypt);
	mutex_unlock(&spacc_hw_lock);

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

	for (i = 15; i >= 0; i--) {
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
	unsigned int done = 0;
	u8 iv[AES_BLOCK_SIZE], next_iv[AES_BLOCK_SIZE];
	int err = 0;

	if (req->cryptlen == 0)
		goto finalize;

	/* AES-192 is not supported by the engine. */
	if (unlikely(ctx->use_fallback)) {
		err = spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				     req->iv, rctx->is_encrypt, req->base.flags);
		goto finalize;
	}

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

		src = scatterwalk_ffwd(src_chain, req->src, done);
		if (req->src == req->dst)
			dst = src;
		else
			dst = scatterwalk_ffwd(dst_chain, req->dst, done);

		chunk = min_t(unsigned int, left, SPACC_MAX_CHUNK_BYTES);
		chunk = spacc_sg_span(src, chunk, SPACC_MAX_DDT_ENTS - 1);
		if (dst != src)
			chunk = spacc_sg_span(dst, chunk, SPACC_MAX_DDT_ENTS - 1);
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
			if (!ctx->is_ctr && !rctx->is_encrypt && chunk < left &&
			    ivsize)
				scatterwalk_map_and_copy(next_iv, src,
							 chunk - ivsize, ivsize, 0);

			err = spacc_hw_chunk(spacc, ctx, src, dst, chunk, ivsize,
					     iv, rctx->is_encrypt);
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
		} else if (done < req->cryptlen && ivsize) {
			/* Next chunk starts with the ciphertext block just produced. */
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

	/*
	 * CTR has no fallback at init time (see cra_init); allocate it here,
	 * where the crypto manager is free, so that small requests can still be
	 * computed in software.
	 */
	if (!ctx->fallback_tfm) {
		ctx->fallback_tfm = crypto_alloc_sync_skcipher(
			crypto_tfm_alg_name(&tfm->base), 0,
			CRYPTO_ALG_NEED_FALLBACK);
		if (IS_ERR(ctx->fallback_tfm)) {
			int err = PTR_ERR(ctx->fallback_tfm);

			ctx->fallback_tfm = NULL;
			dev_err(ctx->spacc->dev,
				"Cannot allocate fallback for %s: %d\n",
				crypto_tfm_alg_name(&tfm->base), err);
			return err;
		}
	}

	if (keylen != AES_KEYSIZE_128 && keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_256)
		return -EINVAL;

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	/* If 192-bit AES is requested and not natively supported, fallback */
	ctx->use_fallback = (keylen == AES_KEYSIZE_192);

	if (ctx->fallback_tfm) {
		crypto_sync_skcipher_clear_flags(ctx->fallback_tfm,
						 CRYPTO_TFM_REQ_MASK);
		crypto_sync_skcipher_set_flags(ctx->fallback_tfm,
					       crypto_skcipher_get_flags(tfm) &
					       CRYPTO_TFM_REQ_MASK);
		return crypto_sync_skcipher_setkey(ctx->fallback_tfm, key,
						   keylen);
	}

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
	 * CTR is a stream mode and the algorithm advertises blocksize 1, so a
	 * length that is not a multiple of 16 is legitimate.  The engine works
	 * in blocks, therefore such a request is computed in software.
	 */
	if (req->cryptlen % AES_BLOCK_SIZE) {
		if (!ctx->is_ctr)
			return -EINVAL;
		return spacc_sw_crypt(ctx, req->src, req->dst, req->cryptlen,
				      req->iv, is_encrypt, req->base.flags);
	}

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


/* ------------------------------------------------------- hash / MAC --- */

/* Largest message buffered for one hardware job; SMB packets are smaller. */
#define SPACC_HASH_MAX_MSG	(256 * 1024)

/* hash alg ids from the vendor tables */
#define SPACC_H_SHA1		2
#define SPACC_H_SHA256		4
#define SPACC_H_CMAC		8

static struct spacc_dev *spacc_hash_dev;

struct spacc_hash_ctx {
	u8 key[64];
	unsigned int keylen;
	unsigned int alg_id;
	unsigned int digestsize;
	bool keyed;
	/* below min_hw the software implementation is cheaper */
	struct crypto_shash *fb;
	unsigned int min_hw;
};

struct spacc_hash_desc {
	u8 *buf;
	unsigned int len;
	unsigned int alloc;
};

static void spacc_hash_free_desc(struct spacc_hash_desc *d)
{
	kfree(d->buf);
	d->buf = NULL;
	d->len = 0;
	d->alloc = 0;
}

/*
 * One job over the whole message: message as pre-AAD, digest into dst, ICV_LEN
 * zero.  ICV_PT (bit 26) and ENCRYPT (bit 24) are required.  KEY_SZ carries the
 * hash key size, or zero for unkeyed algorithms -- it must be written every
 * time, otherwise a previous keyed job leaves its size behind.
 */
static int spacc_hash_hw(struct spacc_hash_ctx *hctx, const u8 *msg,
			 unsigned int len, u8 *out)
{
	struct spacc_dev *spacc = spacc_hash_dev;
	struct sg_table st;
	struct scatterlist dsg;
	struct spacc_ddt *ddt_src, *ddt_dst;
	dma_addr_t a_src, a_dst;
	int n_src = 0, n_dst = 0;
	struct page **pages;
	unsigned int npages, ctx_id;
	void __iomem *page;
	void *dig;
	long left;
	u32 ctrl;
	int err = 0, i;

	if (!spacc || !len)
		return -EINVAL;

	npages = DIV_ROUND_UP(offset_in_page(msg) + len, PAGE_SIZE);
	pages = kmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	dig = kzalloc(64, GFP_KERNEL);
	if (!pages || !dig) {
		err = -ENOMEM;
		goto out;
	}
	for (i = 0; i < npages; i++)
		pages[i] = virt_to_page(msg + i * PAGE_SIZE);

	err = sg_alloc_table_from_pages(&st, pages, npages, offset_in_page(msg),
					len, GFP_KERNEL);
	if (err)
		goto out;

	mutex_lock(&spacc_hw_lock);

	ddt_src = spacc_sg_to_ddt(spacc, st.sgl, len, DMA_TO_DEVICE, &a_src,
				  &n_src);
	if (!ddt_src) {
		err = -ENOMEM;
		goto out_unlock;
	}

	sg_init_one(&dsg, dig, 64);
	ddt_dst = spacc_sg_to_ddt(spacc, &dsg, 64, DMA_FROM_DEVICE, &a_dst,
				  &n_dst);
	if (!ddt_dst) {
		dma_unmap_sg(spacc->dev, st.sgl, n_src, DMA_TO_DEVICE);
		dma_pool_free(spacc->req_pool, ddt_src, a_src);
		err = -ENOMEM;
		goto out_unlock;
	}

	ctx_id = (spacc->next_ctx++) & 0x1f;

	if (hctx->keyed) {
		page = spacc->crypto_regs + SPACC_REG_HASH_KEY_BASE;
		spacc_memcpy_toio32(page + (ctx_id * spacc->cipher_pg_sz),
				    hctx->key, 16);
		spacc_memcpy_toio32(page, hctx->key, 16);
	}
	writel((hctx->keyed ? 16 : 0) | (ctx_id << SPA_KEY_SZ_CTX_INDEX_OFFSET),
	       spacc->crypto_regs + SPACC_REG_KEY_SZ);

	writel(a_src, spacc->crypto_regs + SPACC_REG_SRC_PTR);
	writel(a_dst, spacc->crypto_regs + SPACC_REG_DST_PTR);
	writel(0, spacc->crypto_regs + SPACC_REG_OFFSET);

	writel(len, spacc->crypto_regs + SPACC3_REG_PRE_AAD_LEN);
	writel(0, spacc->crypto_regs + SPACC3_REG_POST_AAD_LEN);
	writel(len, spacc->crypto_regs + SPACC3_REG_PROC_LEN);
	writel(0, spacc->crypto_regs + SPACC3_REG_ICV_LEN);
	writel(0, spacc->crypto_regs + SPACC3_REG_ICV_OFFSET);
	writel(0, spacc->crypto_regs + SPACC3_REG_IV_OFFSET);
	writel(0, spacc->crypto_regs + SPACC3_REG_SW_ID);
	writel(0, spacc->crypto_regs + SPACC3_REG_AUX_INFO);

	ctrl = (hctx->alg_id << 4) | (ctx_id << SPA_CTRL_CTX_IDX) |
	       BIT(14) | BIT(15) | BIT(24) | BIT(26);

	reinit_completion(&spacc->done);
	spacc_write_ctrl(spacc, ctrl);
	left = wait_for_completion_timeout(&spacc->done,
					   msecs_to_jiffies(1000));
	if (!left)
		err = -ETIMEDOUT;
	else
		err = spacc->req_err;

	dma_rmb();
	if (!err)
		memcpy(out, dig, hctx->digestsize);

	dma_unmap_sg(spacc->dev, &dsg, n_dst, DMA_FROM_DEVICE);
	dma_unmap_sg(spacc->dev, st.sgl, n_src, DMA_TO_DEVICE);
	dma_pool_free(spacc->req_pool, ddt_dst, a_dst);
	dma_pool_free(spacc->req_pool, ddt_src, a_src);
out_unlock:
	mutex_unlock(&spacc_hw_lock);
	sg_free_table(&st);
out:
	kfree(pages);
	kfree_sensitive(dig);
	return err;
}

static int spacc_hash_sw(struct spacc_hash_ctx *ctx, const u8 *data,
			 unsigned int len, u8 *out)
{
	SHASH_DESC_ON_STACK(desc, ctx->fb);

	desc->tfm = ctx->fb;
	return crypto_shash_digest(desc, data, len, out);
}

static int spacc_hash_setkey(struct crypto_shash *tfm, const u8 *key,
			     unsigned int keylen)
{
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(tfm);

	if (!ctx->keyed || keylen != AES_KEYSIZE_128)
		return -EINVAL;
	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;
	if (ctx->fb)
		return crypto_shash_setkey(ctx->fb, key, keylen);
	return 0;
}

static int spacc_hash_init(struct shash_desc *desc)
{
	struct spacc_hash_desc *d = shash_desc_ctx(desc);

	d->len = 0;
	return 0;
}

static int spacc_hash_update(struct shash_desc *desc, const u8 *data,
			     unsigned int len)
{
	struct spacc_hash_desc *d = shash_desc_ctx(desc);
	unsigned int need = d->len + len;
	u8 *p;

	if (need > SPACC_HASH_MAX_MSG)
		return -ENOSPC;
	if (need > d->alloc) {
		unsigned int na = max(d->alloc * 2, need);

		if (na > SPACC_HASH_MAX_MSG)
			na = SPACC_HASH_MAX_MSG;
		p = krealloc(d->buf, na, GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		d->buf = p;
		d->alloc = na;
	}
	memcpy(d->buf + d->len, data, len);
	d->len += len;
	return 0;
}

static int spacc_hash_final(struct shash_desc *desc, u8 *out)
{
	struct spacc_hash_desc *d = shash_desc_ctx(desc);
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(desc->tfm);
	int err;

	if (ctx->keyed && ctx->keylen != AES_KEYSIZE_128)
		err = -EINVAL;
	else if (ctx->fb && d->len < ctx->min_hw)
		err = spacc_hash_sw(ctx, d->buf, d->len, out);
	else
		err = spacc_hash_hw(ctx, d->buf, d->len, out);
	spacc_hash_free_desc(d);

	return err;
}

static int spacc_hash_digest(struct shash_desc *desc, const u8 *data,
			     unsigned int len, u8 *out)
{
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(desc->tfm);
	u8 *tmp;
	int err;

	if (ctx->keyed && ctx->keylen != AES_KEYSIZE_128)
		return -EINVAL;

	if (ctx->fb && len < ctx->min_hw)
		return spacc_hash_sw(ctx, data, len, out);

	/*
	 * The caller's buffer may live in vmalloc space (module .rodata, for
	 * one), where virt_to_page() does not describe its pages.  Copy it.
	 */
	tmp = kmalloc(len, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	memcpy(tmp, data, len);
	err = spacc_hash_hw(ctx, tmp, len, out);
	kfree_sensitive(tmp);

	return err;
}

static void spacc_hash_ctx_init(struct crypto_shash *tfm, unsigned int alg,
				unsigned int ds, bool keyed,
				const char *fb_name, unsigned int min_hw)
{
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(tfm);
	struct crypto_shash *fb;

	ctx->alg_id = alg;
	ctx->digestsize = ds;
	ctx->keyed = keyed;
	ctx->keylen = 0;
	ctx->min_hw = min_hw;

	fb = crypto_alloc_shash(fb_name, 0, 0);
	if (IS_ERR(fb)) {
		pr_warn("ls1024a-spacc: no software fallback %s, hardware only\n",
			fb_name);
		ctx->fb = NULL;
	} else {
		ctx->fb = fb;
	}
}

static void spacc_hash_ctx_exit(struct crypto_shash *tfm)
{
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(tfm);

	if (ctx->fb) {
		crypto_free_shash(ctx->fb);
		ctx->fb = NULL;
	}
}

static int spacc_sha1_init_tfm(struct crypto_shash *tfm)
{
	spacc_hash_ctx_init(tfm, SPACC_H_SHA1, 20, false, "sha1-lib", 16384);
	return 0;
}

static int spacc_sha256_init_tfm(struct crypto_shash *tfm)
{
	spacc_hash_ctx_init(tfm, SPACC_H_SHA256, 32, false, "sha256-lib", 4096);
	return 0;
}

static int spacc_cmac_init_tfm(struct crypto_shash *tfm)
{
	spacc_hash_ctx_init(tfm, SPACC_H_CMAC, 16, true, "cmac(aes-generic)",
			    1024);
	return 0;
}


/* ------------------------------------------------------------ HMAC --- */

static int spacc_hmac_setkey(struct crypto_shash *tfm, const u8 *key,
			     unsigned int keylen)
{
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(tfm);
	struct spacc_hash_ctx sha = {
		.alg_id = SPACC_H_SHA256, .digestsize = 32, .keyed = false,
	};
	u8 digest[32];
	int err;

	memset(ctx->key, 0, sizeof(ctx->key));
	if (keylen > sizeof(ctx->key)) {
		err = spacc_hash_hw(&sha, key, keylen, digest);
		if (err)
			return err;
		memcpy(ctx->key, digest, sizeof(digest));
	} else {
		memcpy(ctx->key, key, keylen);
	}
	ctx->keylen = sizeof(ctx->key);

	if (ctx->fb)
		return crypto_shash_setkey(ctx->fb, key, keylen);
	return 0;
}

static int spacc_hmac_compute(struct spacc_hash_ctx *ctx, const u8 *msg,
			      unsigned int len, u8 *out)
{
	struct spacc_hash_ctx sha = {
		.alg_id = SPACC_H_SHA256, .digestsize = 32, .keyed = false,
	};
	u8 inner[32];
	u8 *buf;
	int i, err;

	if (len > SPACC_HASH_MAX_MSG - 64)
		return -ENOSPC;

	buf = kmalloc(64 + len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < 64; i++)
		buf[i] = ctx->key[i] ^ 0x36;
	memcpy(buf + 64, msg, len);
	err = spacc_hash_hw(&sha, buf, 64 + len, inner);
	kfree_sensitive(buf);
	if (err)
		return err;

	buf = kmalloc(96, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < 64; i++)
		buf[i] = ctx->key[i] ^ 0x5c;
	memcpy(buf + 64, inner, sizeof(inner));
	err = spacc_hash_hw(&sha, buf, 96, out);
	kfree_sensitive(buf);

	return err;
}

static int spacc_hmac_final(struct shash_desc *desc, u8 *out)
{
	struct spacc_hash_desc *d = shash_desc_ctx(desc);
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(desc->tfm);
	int err;

	if (ctx->fb && d->len < ctx->min_hw)
		err = spacc_hash_sw(ctx, d->buf, d->len, out);
	else
		err = spacc_hmac_compute(ctx, d->buf, d->len, out);
	spacc_hash_free_desc(d);

	return err;
}

static int spacc_hmac_digest(struct shash_desc *desc, const u8 *data,
			     unsigned int len, u8 *out)
{
	struct spacc_hash_ctx *ctx = crypto_shash_ctx(desc->tfm);
	u8 *tmp;
	int err;

	if (ctx->fb && len < ctx->min_hw)
		return spacc_hash_sw(ctx, data, len, out);

	tmp = kmalloc(len, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	memcpy(tmp, data, len);
	err = spacc_hmac_compute(ctx, tmp, len, out);
	kfree_sensitive(tmp);

	return err;
}

static int spacc_hmac_init_tfm(struct crypto_shash *tfm)
{
	spacc_hash_ctx_init(tfm, SPACC_H_SHA256, 32, false,
			    "hmac-sha256-lib", 16384);
	return 0;
}

static struct shash_alg spacc_hash_algs[] = {
	{
		.digestsize	= 20,
		.init		= spacc_hash_init,
		.update		= spacc_hash_update,
		.final		= spacc_hash_final,
		.digest		= spacc_hash_digest,
		.descsize	= sizeof(struct spacc_hash_desc),
		.init_tfm	= spacc_sha1_init_tfm,
		.exit_tfm	= spacc_hash_ctx_exit,
		.base		= {
			.cra_name	= "sha1",
			.cra_driver_name = "sha1-ls1024a-spacc",
			.cra_priority	= 300,
			.cra_blocksize	= 64,
			.cra_ctxsize	= sizeof(struct spacc_hash_ctx),
			.cra_module	= THIS_MODULE,
		},
	},
	{
		.digestsize	= 32,
		.init		= spacc_hash_init,
		.update		= spacc_hash_update,
		.final		= spacc_hash_final,
		.digest		= spacc_hash_digest,
		.descsize	= sizeof(struct spacc_hash_desc),
		.init_tfm	= spacc_sha256_init_tfm,
		.exit_tfm	= spacc_hash_ctx_exit,
		.base		= {
			.cra_name	= "sha256",
			.cra_driver_name = "sha256-ls1024a-spacc",
			.cra_priority	= 300,
			.cra_blocksize	= 64,
			.cra_ctxsize	= sizeof(struct spacc_hash_ctx),
			.cra_module	= THIS_MODULE,
		},
	},
	{
		.digestsize	= 32,
		.init		= spacc_hash_init,
		.update		= spacc_hash_update,
		.final		= spacc_hmac_final,
		.digest		= spacc_hmac_digest,
		.setkey		= spacc_hmac_setkey,
		.descsize	= sizeof(struct spacc_hash_desc),
		.init_tfm	= spacc_hmac_init_tfm,
		.exit_tfm	= spacc_hash_ctx_exit,
		.base		= {
			.cra_name	= "hmac(sha256)",
			.cra_driver_name = "hmac-sha256-ls1024a-spacc",
			.cra_priority	= 300,
			.cra_blocksize	= 64,
			.cra_ctxsize	= sizeof(struct spacc_hash_ctx),
			.cra_module	= THIS_MODULE,
		},
	},
	{
		.digestsize	= 16,
		.init		= spacc_hash_init,
		.update		= spacc_hash_update,
		.final		= spacc_hash_final,
		.digest		= spacc_hash_digest,
		.setkey		= spacc_hash_setkey,
		.descsize	= sizeof(struct spacc_hash_desc),
		.init_tfm	= spacc_cmac_init_tfm,
		.exit_tfm	= spacc_hash_ctx_exit,
		.base		= {
			.cra_name	= "cmac(aes)",
			.cra_driver_name = "cmac-aes-ls1024a-spacc",
			.cra_priority	= 300,
			.cra_blocksize	= 16,
			.cra_ctxsize	= sizeof(struct spacc_hash_ctx),
			.cra_module	= THIS_MODULE,
		},
	},
};
static int spacc_probe(struct platform_device *pdev)
{
	struct spacc_dev *spacc;
	struct resource *res;
	int ret, i;
	u32 spacc_id, major;

	spacc = devm_kzalloc(&pdev->dev, sizeof(*spacc), GFP_KERNEL);
	if (!spacc)
		return -ENOMEM;

	spacc->dev = &pdev->dev;
	init_completion(&spacc->done);

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
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	spacc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spacc->regs)) {
		ret = PTR_ERR(spacc->regs);
		goto err_reset_assert;
	}

	/*
	 * If the mapped window is large enough (>= 0x48000), crypto registers
	 * reside at SPACC_CRYPTO_BASE_OFFSET (0x40000). Otherwise, if a 64KB
	 * region was mapped, the core registers reside at offset 0.
	 */
	if (resource_size(res) >= 0x48000)
		spacc->crypto_regs = spacc->regs + SPACC_CRYPTO_BASE_OFFSET;
	else
		spacc->crypto_regs = spacc->regs;

	spacc->pdu_regs = spacc->regs;
	spacc->cipher_ctx_base = spacc->crypto_regs + SPACC_REG_CIPH_KEY_BASE;
	spacc->cipher_pg_sz = SPACC_CIPHER_PG_SZ;

	/* Detect hardware version */
	spacc_id = readl(spacc->pdu_regs + SPACC_REG_ID);
	major = (spacc_id >> 4) & 0x0F;
	if (major >= 3 || of_device_is_compatible(pdev->dev.of_node, "fsl,ls1024a-spacc") ||
	    of_device_is_compatible(pdev->dev.of_node, "picochip,spacc-ls1024a"))
		spacc->is_spacc3 = true;
	else
		spacc->is_spacc3 = false;

	dev_info(&pdev->dev, "SPACC crypto engine probed (version %u.%u, mode %s)\n",
		 major, spacc_id & 0x0F, spacc->is_spacc3 ? "SPACC 3.x" : "SPACC 2.x");

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

	ret = devm_request_irq(&pdev->dev, spacc->irq, spacc_irq_handler, 0,
			       dev_name(&pdev->dev), spacc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", spacc->irq, ret);
		goto err_reset_assert;
	}

	/* Clear pending interrupts */
	writel(~0, spacc->pdu_regs + SPACC_REG_IRQ_STAT);
	if (spacc->crypto_regs != spacc->pdu_regs)
		writel(~0, spacc->crypto_regs + SPACC_REG_IRQ_STAT);

	/*
	 * Set IRQ threshold to 1 and enable the status interrupt only: the
	 * command-FIFO interrupt carries no completion information.
	 */
	writel(1 << SPA_IRQ_CTRL_STAT_CNT_OFFSET, spacc->crypto_regs + SPACC_REG_IRQ_CTRL);
	writel(SPA_IRQ_EN_STAT_EN | SPA_IRQ_EN_GLBL_EN,
	       spacc->pdu_regs + SPACC_REG_IRQ_EN);

	/* Initialize crypto engine */
	spacc->ce = crypto_engine_alloc_init(&pdev->dev, true);
	if (!spacc->ce) {
		ret = -ENOMEM;
		goto err_irq_disable;
	}

	ret = crypto_engine_start(spacc->ce);
	if (ret)
		goto err_ce_exit;

	platform_set_drvdata(pdev, spacc);

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
			goto err_ce_stop;
		}
	}

	{
		int h;
		bool ok = true;

		for (h = 0; h < ARRAY_SIZE(spacc_hash_algs); h++) {
			if (crypto_register_shash(&spacc_hash_algs[h])) {
				dev_warn(&pdev->dev, "hash %s not registered\n",
					 spacc_hash_algs[h].base.cra_name);
				while (--h >= 0)
					crypto_unregister_shash(
						&spacc_hash_algs[h]);
				ok = false;
				break;
			}
		}
		if (ok)
			spacc_hash_dev = spacc;
	}

	return 0;

err_ce_stop:
	crypto_engine_stop(spacc->ce);
err_ce_exit:
	crypto_engine_exit(spacc->ce);
err_irq_disable:
	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);
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

	if (spacc_hash_dev) {
		for (i = 0; i < ARRAY_SIZE(spacc_hash_algs); i++)
			crypto_unregister_shash(&spacc_hash_algs[i]);
		spacc_hash_dev = NULL;
	}

	for (i = 0; i < ARRAY_SIZE(spacc->algs); i++)
		crypto_engine_unregister_skcipher(&spacc->algs[i].alg);

	crypto_engine_stop(spacc->ce);
	crypto_engine_exit(spacc->ce);

	writel(0, spacc->pdu_regs + SPACC_REG_IRQ_EN);

	if (spacc->rst)
		reset_control_assert(spacc->rst);

	if (spacc->clk)
		clk_disable_unprepare(spacc->clk);
}

static const struct of_device_id spacc_of_match[] = {
	{ .compatible = "fsl,ls1024a-spacc", },
	{ .compatible = "picochip,spacc-ls1024a", },
	{ .compatible = "picochip,spacc-ipsec", },
	{ .compatible = "picochip,spacc-l2", },
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

MODULE_DESCRIPTION("Freescale LS1024A / Mindspeed SPACC Crypto Engine Driver");
MODULE_AUTHOR("Antigravity Team");
MODULE_LICENSE("GPL");
