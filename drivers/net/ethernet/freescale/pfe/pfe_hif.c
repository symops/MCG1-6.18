// SPDX-License-Identifier: GPL-2.0
/*
 * HIF (Host Interface) DMA descriptor ring + ISR -- ported from the
 * 3.2.26 vendor tree's pfe_ctrl/pfe_hif.c (kmodules/mspd-c2k/pfe/ in
 * symops/MCG1-3.2.26). This is the highest-risk file in the whole PFE
 * port: a fully custom host<->firmware DMA/descriptor protocol with no
 * mainline equivalent to crib from.
 *
 * Dropped, not deferred, relative to the vendor file:
 *  - CONFIG_PLATFORM_PCI and CONFIG_PLATFORM_EMULATION branches: this
 *    board is neither.
 *  - page_mode/LRO buffer allocation: a performance feature, not needed
 *    for basic connectivity (see pfe_hif_lib.c). pfe_pkt_size/
 *    pfe_pkt_headroom are fixed at their non-LRO values.
 *  - The __memcpy8()/__memcpy12() hand-written ARM ldm/stm helpers:
 *    replaced with plain memcpy() of the same fixed sizes. The
 *    descriptor fields are already `volatile`, and the ring itself is
 *    dma_alloc_coherent() memory, so there's no correctness reason for
 *    hand-rolled assembly here -- it bought a minor, unmeasured
 *    micro-optimization on 2012-era compilers at the cost of being
 *    silently wrong if the struct layouts it assumes ever changed.
 *  - outer_inv_range() on the descriptor ring + the napi_first_batch
 *    flag that gated it: this was an explicit PL310 outer-cache
 *    invalidate on the *descriptor* memory specifically (not the
 *    packet buffers, which already go through the ordinary
 *    dma_unmap_single()/dma_map_single() DMA API further down in this
 *    same function). dma_alloc_coherent() memory is coherent by
 *    definition -- if this SoC's descriptor ring genuinely needs
 *    manual cache maintenance on top of that, dma_alloc_coherent()
 *    would be the wrong allocator to begin with. Dropped; flagged here
 *    in case real-hardware Rx corruption ever points back at it.
 *
 * Deferred to Stage P7-P9 (pfe_eth.c, the only caller of any of these):
 * the client TX path (__hif_xmit_pkt() and hif_xmit_pkt() are ported
 * since they're small and self-contained, but nothing calls them yet).
 */

#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/string.h>

#include "pfe_mod.h"
#include "pfe_hif.h"
#include "pfe_hif_lib.h"
#include "pfe_hw_lib.h"

#define HIF_INT_MASK		(HIF_INT | HIF_RXPKT_INT)
#define TX_FREE_MAX_COUNT	64

#define inc_hif_rxidx(idxname)	idxname = (idxname + 1) & (hif->RxRingSize - 1)
#define inc_hif_txidx(idxname)	idxname = (idxname + 1) & (hif->TxRingSize - 1)

static int pfe_hif_alloc_descr(struct pfe_hif *hif)
{
	void *addr;
	dma_addr_t dma_addr;

	addr = dma_alloc_coherent(hif->dev,
				   HIF_RX_DESC_NT * sizeof(struct hif_desc) +
				   HIF_TX_DESC_NT * sizeof(struct hif_desc),
				   &dma_addr, GFP_KERNEL);
	if (!addr) {
		dev_err(hif->dev, "%s: could not allocate buffer descriptors\n", __func__);
		return -ENOMEM;
	}

	hif->descr_baseaddr_p = dma_addr;
	hif->descr_baseaddr_v = addr;
	hif->RxRingSize = HIF_RX_DESC_NT;
	hif->TxRingSize = HIF_TX_DESC_NT;

	addr = dma_alloc_coherent(hif->dev, HIF_TX_DESC_NT * sizeof(struct hif_tso_hdr),
				   &dma_addr, GFP_KERNEL);
	if (!addr) {
		dev_err(hif->dev, "%s: could not allocate per-packet tx header\n", __func__);
		dma_free_coherent(hif->dev,
				   hif->RxRingSize * sizeof(struct hif_desc) +
				   hif->TxRingSize * sizeof(struct hif_desc),
				   hif->descr_baseaddr_v, hif->descr_baseaddr_p);
		return -ENOMEM;
	}

	hif->tso_hdr_p = dma_addr;
	hif->tso_hdr_v = addr;

	return 0;
}

static void pfe_hif_free_descr(struct pfe_hif *hif)
{
	dma_free_coherent(hif->dev, hif->TxRingSize * sizeof(struct hif_tso_hdr),
			   hif->tso_hdr_v, hif->tso_hdr_p);
	dma_free_coherent(hif->dev,
			   hif->RxRingSize * sizeof(struct hif_desc) +
			   hif->TxRingSize * sizeof(struct hif_desc),
			   hif->descr_baseaddr_v, hif->descr_baseaddr_p);
}

static void pfe_hif_release_buffers(struct pfe_hif *hif)
{
	struct hif_desc *desc = hif->RxBase;
	int i;

	for (i = 0; i < hif->RxRingSize; i++) {
		if (desc->data) {
			if (i < hif->shm->rx_buf_pool_cnt && !hif->shm->rx_buf_pool[i]) {
				dma_unmap_single(hif->dev, desc->data, pfe_pkt_size,
						  DMA_FROM_DEVICE);
				hif->shm->rx_buf_pool[i] = hif->rx_buf_addr[i];
			} else {
				dev_err(hif->dev, "%s: buffer pool already full\n", __func__);
			}
		}

		desc->data = 0;
		desc->status = 0;
		desc->ctrl = 0;
		desc++;
	}
}

/*
 * Initializes the HIF Rx/Tx ring descriptors and fills the Rx ring with
 * buffers from the shared-memory pool.
 */
static int pfe_hif_init_buffers(struct pfe_hif *hif)
{
	struct hif_desc *desc, *first_desc_p;
	dma_addr_t data;
	int i;

	if (hif->shm->rx_buf_pool_cnt < hif->RxRingSize)
		return -ENOMEM;

	hif->RxBase = hif->descr_baseaddr_v;
	memset(hif->RxBase, 0, hif->RxRingSize * sizeof(struct hif_desc));

	desc = hif->RxBase;
	first_desc_p = (struct hif_desc *)hif->descr_baseaddr_p;

	for (i = 0; i < hif->RxRingSize; i++) {
		data = dma_map_single(hif->dev, (void *)hif->shm->rx_buf_pool[i],
				       pfe_pkt_size, DMA_FROM_DEVICE);
		hif->rx_buf_addr[i] = (void *)hif->shm->rx_buf_pool[i];
		hif->shm->rx_buf_pool[i] = NULL;

		if (unlikely(dma_mapping_error(hif->dev, data))) {
			dev_err(hif->dev, "%s: low on mem\n", __func__);
			goto err;
		}
		desc->data = data;

		desc->status = 0;
		wmb();
		desc->ctrl = BD_CTRL_PKT_INT_EN | BD_CTRL_LIFM | BD_CTRL_DIR |
			     BD_CTRL_DESC_EN | BD_BUF_LEN(pfe_pkt_size);
		desc->next = (u32)(first_desc_p + i + 1);
		desc++;
	}
	desc--;
	desc->next = (u32)first_desc_p;

	hif->RxtocleanIndex = 0;

	writel(hif->descr_baseaddr_p, HIF_RX_BDP_ADDR);

	hif->TxBase = hif->RxBase + hif->RxRingSize;
	first_desc_p = (struct hif_desc *)hif->descr_baseaddr_p + hif->RxRingSize;
	memset(hif->TxBase, 0, hif->TxRingSize * sizeof(struct hif_desc));

	desc = hif->TxBase;
	for (i = 0; i < hif->TxRingSize; i++) {
		desc->next = (u32)(first_desc_p + i + 1);
		desc++;
	}
	desc--;
	desc->next = (u32)first_desc_p;

	hif->TxAvail = hif->TxRingSize;
	hif->Txtosend = 0;
	hif->Txtoclean = 0;

	writel((u32)first_desc_p, HIF_TX_BDP_ADDR);

	return 0;

err:
	pfe_hif_release_buffers(hif);
	return -ENOMEM;
}

static int pfe_hif_client_register(struct pfe_hif *hif, u32 client_id,
				    struct hif_client_shm *client_shm)
{
	struct hif_client *client = &hif->client[client_id];
	struct rx_queue_desc *rx_qbase;
	struct tx_queue_desc *tx_qbase;
	u32 i, cnt;

	spin_lock_bh(&hif->lock);
	spin_lock_bh(&hif->tx_lock);

	if (hif->shm->gClient_status & (1 << client_id)) {
		dev_err(hif->dev, "%s: client %d already registered\n", __func__, client_id);
		spin_unlock_bh(&hif->tx_lock);
		spin_unlock_bh(&hif->lock);
		return -EINVAL;
	}

	memset(client, 0, sizeof(*client));

	cnt = min_t(u32, CLIENT_CTRL_RX_Q_CNT(client_shm->ctrl), HIF_CLIENT_QUEUES_MAX);
	client->rx_qn = cnt;
	rx_qbase = (struct rx_queue_desc *)client_shm->rx_qbase;
	for (i = 0; i < cnt; i++) {
		client->rx_q[i].base = rx_qbase + i * client_shm->rx_qsize;
		client->rx_q[i].size = client_shm->rx_qsize;
		client->rx_q[i].write_idx = 0;
	}

	cnt = min_t(u32, CLIENT_CTRL_TX_Q_CNT(client_shm->ctrl), HIF_CLIENT_QUEUES_MAX);
	client->tx_qn = cnt;
	tx_qbase = (struct tx_queue_desc *)client_shm->tx_qbase;
	for (i = 0; i < cnt; i++) {
		client->tx_q[i].base = tx_qbase + i * client_shm->tx_qsize;
		client->tx_q[i].size = client_shm->tx_qsize;
		client->tx_q[i].ack_idx = 0;
	}

	hif->shm->gClient_status |= (1 << client_id);

	spin_unlock_bh(&hif->tx_lock);
	spin_unlock_bh(&hif->lock);

	return 0;
}

static void pfe_hif_client_unregister(struct pfe_hif *hif, u32 client_id)
{
	spin_lock_bh(&hif->lock);
	spin_lock_bh(&hif->tx_lock);

	if (!(hif->shm->gClient_status & (1 << client_id))) {
		dev_err(hif->dev, "%s: client %d not registered\n", __func__, client_id);
	} else {
		hif->shm->gClient_status &= ~(1 << client_id);
	}

	spin_unlock_bh(&hif->tx_lock);
	spin_unlock_bh(&hif->lock);
}

/*
 * Puts the Rx packet in the given client's Rx queue, swapping in a
 * freshly allocated buffer and returning the one that came out (which
 * the caller then re-posts into the HIF descriptor). Returns NULL if
 * the client's Rx queue is full.
 */
static void *client_put_rxpacket(struct pfe_hif *hif, void *pkt, u32 len, u32 flags,
				  u32 client_ctrl)
{
	struct hif_rx_queue *queue = &hif->client[hif->client_id].rx_q[hif->qno];
	struct rx_queue_desc *desc = queue->base + queue->write_idx;
	void *free_pkt = NULL;

	if (!(desc->ctrl & CL_DESC_OWN))
		return NULL;

	free_pkt = kmalloc(PFE_BUF_SIZE, GFP_ATOMIC);
	if (!free_pkt)
		return NULL;

	desc->data = pkt;
	desc->client_ctrl = client_ctrl;
	smp_wmb();
	desc->ctrl = CL_DESC_BUF_LEN(len) | flags;
	queue->write_idx = (queue->write_idx + 1) & (queue->size - 1);

	return free_pkt + pfe_pkt_headroom;
}

/*
 * Dequeues HIF Rx descriptors and hands completed packets to the owning
 * client's Rx queue. NAPI poll function.
 */
static int pfe_hif_rx_process(struct pfe_hif *hif, int budget)
{
	struct hif_hdr pkt_hdr;
	void *pkt_hdr_ptr;
	void *free_buf;
	int rtc, len, rx_processed = 0;
	struct __hif_desc local_desc;
	struct hif_desc *desc;
	int flags;

	spin_lock_bh(&hif->lock);

	rtc = hif->RxtocleanIndex;

	while (rx_processed < budget) {
		desc = hif->RxBase + rtc;

		memcpy(&local_desc, desc, sizeof(local_desc));
		if (local_desc.ctrl & BD_CTRL_DESC_EN) {
			writel(HIF_INT_MASK, HIF_INT_SRC);
			memcpy(&local_desc, desc, sizeof(local_desc));
			if (local_desc.ctrl & BD_CTRL_DESC_EN)
				break;
		}

		hif->napi_counters[NAPI_DESC_COUNT]++;

		len = BD_BUF_LEN(local_desc.ctrl);
		dma_unmap_single(hif->dev, local_desc.data, pfe_pkt_size, DMA_FROM_DEVICE);

		pkt_hdr_ptr = hif->rx_buf_addr[rtc];

		/* Track the last HIF header received, for multi-buffer packets. */
		if (!hif->started) {
			hif->started = 1;
			memcpy(&pkt_hdr, pkt_hdr_ptr, sizeof(pkt_hdr));
			hif->qno = pkt_hdr.qNo;
			hif->client_id = pkt_hdr.client_id;
			hif->client_ctrl = (pkt_hdr.client_ctrl1 << 16) | pkt_hdr.client_ctrl;
			flags = CL_DESC_FIRST;
		} else {
			flags = 0;
		}

		if (local_desc.ctrl & BD_CTRL_LIFM)
			flags |= CL_DESC_LAST;

		if (hif->client_id >= HIF_CLIENTS_MAX ||
		    !(hif->shm->gClient_status & (1 << hif->client_id))) {
			dev_err(hif->dev, "%s: packet with invalid client id %d qNo %d\n",
				__func__, hif->client_id, hif->qno);
			free_buf = pkt_hdr_ptr;
			goto pkt_drop;
		}

		if (hif->client[hif->client_id].rx_qn <= hif->qno) {
			dev_info(hif->dev, "%s: packet with invalid queue: %d\n",
				 __func__, hif->qno);
			hif->qno = 0;
		}

		free_buf = client_put_rxpacket(hif, pkt_hdr_ptr, len, flags, hif->client_ctrl);

		hif_lib_indicate_client(hif->client_id, EVENT_RX_PKT_IND, hif->qno);

		if (unlikely(!free_buf)) {
			hif->napi_counters[NAPI_CLIENT_FULL_COUNT]++;
			/*
			 * Tell NAPI we consumed the full budget so it keeps
			 * polling us instead of a livelock where this
			 * instance stays at the head of the list forever.
			 */
			rx_processed = budget;
			if (flags & CL_DESC_FIRST)
				hif->started = 0;
			break;
		}

pkt_drop:
		hif->rx_buf_addr[rtc] = free_buf;
		desc->data = dma_map_single(hif->dev, free_buf, pfe_pkt_size, DMA_FROM_DEVICE);
		wmb();
		desc->ctrl = BD_CTRL_PKT_INT_EN | BD_CTRL_LIFM | BD_CTRL_DIR |
			     BD_CTRL_DESC_EN | BD_BUF_LEN(pfe_pkt_size);

		inc_hif_rxidx(rtc);

		if (local_desc.ctrl & BD_CTRL_LIFM) {
			if (!(hif->client_ctrl & HIF_CTRL_RX_CONTINUED)) {
				rx_processed++;
				hif->napi_counters[NAPI_PACKET_COUNT]++;
			}
			hif->started = 0;
		}
	}

	hif->RxtocleanIndex = rtc;
	spin_unlock_bh(&hif->lock);

	/* We made progress; re-start Rx DMA in case it stopped. */
	hif_rx_dma_start();

	return rx_processed;
}

static int client_ack_txpacket(struct pfe_hif *hif, unsigned int client_id, unsigned int q_no)
{
	struct hif_tx_queue *queue = &hif->client[client_id].tx_q[q_no];
	struct tx_queue_desc *desc = queue->base + queue->ack_idx;

	if (!(desc->ctrl & CL_DESC_OWN)) {
		dev_err(hif->dev, "%s: unexpected: %d %d %d %d %d %p %d\n", __func__,
			hif->Txtosend, hif->Txtoclean, hif->TxAvail, client_id, q_no,
			queue, queue->ack_idx);
		return -EINVAL;
	}

	desc->ctrl &= ~CL_DESC_OWN;
	queue->ack_idx = (queue->ack_idx + 1) & (queue->size - 1);

	return 0;
}

void __hif_tx_done_process(struct pfe_hif *hif, int count)
{
	struct hif_desc *desc;
	struct hif_desc_sw *desc_sw;
	int ttc, tx_avl;

	ttc = hif->Txtoclean;
	tx_avl = hif->TxAvail;

	while (tx_avl < hif->TxRingSize && count--) {
		desc = hif->TxBase + ttc;
		if (desc->ctrl & BD_CTRL_DESC_EN)
			break;

		desc_sw = &hif->tx_sw_queue[ttc];
		if (desc_sw->data)
			dma_unmap_single(hif->dev, desc_sw->data, desc_sw->len, DMA_TO_DEVICE);

		client_ack_txpacket(hif, desc_sw->client_id, desc_sw->q_no);

		inc_hif_txidx(ttc);
		tx_avl++;
	}

	hif->Txtoclean = ttc;
	hif->TxAvail = tx_avl;
}

/* Puts one packet in the HIF Tx ring. Not yet called by anything --
 * kept here (rather than in Stage P7-P9) because it's small,
 * self-contained, and belongs with the rest of the Tx ring management
 * in this file.
 */
void __hif_xmit_pkt(struct pfe_hif *hif, unsigned int client_id, unsigned int q_no,
		     void *data, u32 len, unsigned int flags)
{
	struct hif_desc *desc = hif->TxBase + hif->Txtosend;
	struct hif_desc_sw *desc_sw = &hif->tx_sw_queue[hif->Txtosend];

	desc_sw->len = len;
	desc_sw->client_id = client_id;
	desc_sw->q_no = q_no;

	if (flags & HIF_DONT_DMA_MAP) {
		desc_sw->data = 0;
		desc->data = (u32)data;
	} else {
		desc_sw->data = dma_map_single(hif->dev, data, len, DMA_TO_DEVICE);
		desc->data = (u32)desc_sw->data;
	}

	wmb();

	if (flags & HIF_LAST_BUFFER) {
		if (client_id != PFE_CL_VWD)
			desc->ctrl = BD_CTRL_LIFM | BD_CTRL_BRFETCH_DISABLE |
				     BD_CTRL_RTFETCH_DISABLE | BD_CTRL_PARSE_DISABLE |
				     BD_CTRL_DESC_EN | BD_BUF_LEN(len);
		else
			desc->ctrl = BD_CTRL_LIFM | BD_CTRL_DESC_EN | BD_BUF_LEN(len);
	} else {
		desc->ctrl = BD_CTRL_DESC_EN | BD_BUF_LEN(len);
	}

	inc_hif_txidx(hif->Txtosend);
	hif->TxAvail--;
}

int hif_xmit_pkt(struct pfe_hif *hif, unsigned int client_id, unsigned int q_no,
		  void *data, unsigned int len)
{
	int rc = 0;

	spin_lock_bh(&hif->tx_lock);

	if (!hif->TxAvail) {
		rc = 1;
	} else {
		__hif_xmit_pkt(hif, client_id, q_no, data, len, HIF_FIRST_BUFFER | HIF_LAST_BUFFER);
		hif_tx_dma_start();
	}
	if (hif->TxAvail < (hif->TxRingSize >> 1))
		__hif_tx_done_process(hif, TX_FREE_MAX_COUNT);

	spin_unlock_bh(&hif->tx_lock);

	return rc;
}

static irqreturn_t hif_isr(int irq, void *dev_id)
{
	struct pfe_hif *hif = dev_id;
	int int_status;

	int_status = readl_relaxed(HIF_INT_SRC);

	if (!(int_status & HIF_INT))
		return IRQ_NONE;

	int_status &= ~HIF_INT;

	if (int_status & HIF_RXPKT_INT) {
		int_status &= ~HIF_RXPKT_INT;

		/* Disable interrupts; the NAPI poll re-enables them. */
		writel_relaxed(0, HIF_INT_ENABLE);

		if (napi_schedule_prep(&hif->napi)) {
			hif->napi_counters[NAPI_SCHED_COUNT]++;
			__napi_schedule(&hif->napi);
		}
	}

	if (int_status) {
		dev_info(hif->dev, "%s: unhandled interrupt: %d\n", __func__, int_status);
		writel(int_status, HIF_INT_SRC);
	}

	return IRQ_HANDLED;
}

void hif_process_client_req(struct pfe_hif *hif, int req, int data1, int data2)
{
	unsigned int client_id = data1;

	if (client_id >= HIF_CLIENTS_MAX) {
		dev_err(hif->dev, "%s: client id %d out of bounds\n", __func__, client_id);
		return;
	}

	switch (req) {
	case REQUEST_CL_REGISTER:
		pfe_hif_client_register(hif, client_id,
					 (struct hif_client_shm *)&hif->shm->client[client_id]);
		break;
	case REQUEST_CL_UNREGISTER:
		pfe_hif_client_unregister(hif, client_id);
		break;
	default:
		dev_err(hif->dev, "%s: unsupported request %d\n", __func__, req);
		break;
	}
}

static int pfe_hif_rx_poll(struct napi_struct *napi, int budget)
{
	struct pfe_hif *hif = container_of(napi, struct pfe_hif, napi);
	int work_done;

	hif->napi_counters[NAPI_POLL_COUNT]++;

	work_done = pfe_hif_rx_process(hif, budget);

	if (work_done < budget) {
		napi_complete(napi);
		writel_relaxed(HIF_INT_MASK, HIF_INT_ENABLE);
	} else {
		hif->napi_counters[NAPI_FULL_BUDGET_COUNT]++;
	}

	return work_done;
}

int pfe_hif_init(struct pfe *pfe)
{
	struct pfe_hif *hif = &pfe->hif;
	int err;

	hif->dev = pfe->dev;
	hif->irq = pfe->hif_irq;

	err = pfe_hif_alloc_descr(hif);
	if (err)
		return err;

	err = pfe_hif_init_buffers(hif);
	if (err) {
		dev_err(hif->dev, "%s: could not initialize buffer descriptors\n", __func__);
		goto err_buffers;
	}

	/*
	 * HIF's NAPI instance is shared by all future client net_devices
	 * (GEM0-2, Stage P7-P9), not owned by any single one of them --
	 * needs its own carrier net_device. init_dummy_netdev() (what the
	 * vendor driver used) is no longer available to drivers as of
	 * recent kernels; alloc_netdev_dummy() is the current public
	 * replacement for exactly this (see e.g. mtk_eth_soc.c, a driver
	 * with the same "one engine, several ports" shape as this one).
	 */
	hif->dummy_dev = alloc_netdev_dummy(0);
	if (!hif->dummy_dev) {
		err = -ENOMEM;
		goto err_buffers;
	}

	netif_napi_add(hif->dummy_dev, &hif->napi, pfe_hif_rx_poll);
	napi_enable(&hif->napi);

	spin_lock_init(&hif->tx_lock);
	spin_lock_init(&hif->lock);

	hif_init();
	hif_rx_enable();
	hif_tx_enable();

	/* Disable tx-done interrupt for now (only Rx/global are used). */
	writel(HIF_INT_MASK, HIF_INT_ENABLE);

	gpi_enable(HGPI_BASE_ADDR);

	err = devm_request_irq(hif->dev, hif->irq, hif_isr, 0, "pfe_hif", hif);
	if (err) {
		dev_err(hif->dev, "%s: failed to request hif IRQ = %d\n", __func__, err);
		goto err_irq;
	}

	return 0;

err_irq:
	napi_disable(&hif->napi);
	netif_napi_del(&hif->napi);
	free_netdev(hif->dummy_dev);
err_buffers:
	pfe_hif_free_descr(hif);
	return err;
}

void pfe_hif_exit(struct pfe *pfe)
{
	struct pfe_hif *hif = &pfe->hif;

	hif->shm->gClient_status = 0;

	gpi_disable(HGPI_BASE_ADDR);
	hif_rx_disable();
	hif_tx_disable();

	napi_disable(&hif->napi);
	netif_napi_del(&hif->napi);
	free_netdev(hif->dummy_dev);

	pfe_hif_release_buffers(hif);
	pfe_hif_free_descr(hif);
}
