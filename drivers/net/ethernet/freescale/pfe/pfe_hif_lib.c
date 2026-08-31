// SPDX-License-Identifier: GPL-2.0
/*
 * HIF client-facing shared-memory setup and event indication -- ported
 * from the 3.2.26 vendor tree's pfe_ctrl/pfe_hif_lib.c (kmodules/
 * mspd-c2k/pfe/ in symops/MCG1-3.2.26).
 *
 * Stage P5 added the Rx buffer pool and event indication; Stage P7
 * added client registration; Stage P8 adds the TX submission path and
 * the Rx dequeue/re-arm pair (hif_lib_receive_pkt()/
 * hif_lib_event_handler_start()) -- see pfe_hif_lib.h for what's still
 * out of scope (TSO, TX credit/QoS).
 *
 * page_mode/LRO support is dropped, not deferred: it's a performance
 * feature (larger receive buffers backed by whole pages instead of
 * kmalloc'd blocks) irrelevant to reaching basic connectivity, and
 * carrying its conditional paths through pfe_hif.c/pfe_hif_lib.c would
 * just be more surface area to get wrong for no benefit at this stage.
 * pfe_pkt_size/pfe_pkt_headroom are fixed at their non-LRO values.
 */

#include <linux/kernel.h>
#include <linux/slab.h>

#include "pfe_mod.h"
#include "pfe_hif.h"
#include "pfe_hif_lib.h"

unsigned int pfe_pkt_size = PFE_PKT_SIZE;
unsigned int pfe_pkt_headroom = PFE_PKT_HEADROOM;

/* HIF shared memory -- single instance, matches this SoC only ever
 * having one PFE.
 */
static struct hif_shm ghif_shm;

static void pfe_hif_shm_clean(struct hif_shm *hif_shm)
{
	int i;
	void *pkt;

	for (i = 0; i < hif_shm->rx_buf_pool_cnt; i++) {
		pkt = (void *)hif_shm->rx_buf_pool[i];
		if (pkt) {
			hif_shm->rx_buf_pool[i] = NULL;
			kfree(pkt - pfe_pkt_headroom);
		}
	}
}

/*
 * Allocate the Rx buffer pool the HIF Rx ring is filled from. Must run
 * before pfe_hif_init() (pfe_hif_init_buffers() checks rx_buf_pool_cnt).
 */
static int pfe_hif_shm_init(struct hif_shm *hif_shm)
{
	void *pkt;
	int i;

	memset(hif_shm, 0, sizeof(*hif_shm));
	hif_shm->rx_buf_pool_cnt = HIF_RX_DESC_NT;

	for (i = 0; i < hif_shm->rx_buf_pool_cnt; i++) {
		pkt = kmalloc(PFE_BUF_SIZE, GFP_KERNEL);
		if (!pkt)
			goto err;

		hif_shm->rx_buf_pool[i] = pkt + pfe_pkt_headroom;
	}

	return 0;

err:
	pr_err("%s: low memory\n", __func__);
	pfe_hif_shm_clean(hif_shm);
	return -ENOMEM;
}

void hif_lib_indicate_client(int client_id, int event_type, int qno)
{
	struct hif_client_s *client = g_pfe->hif_client[client_id];

	if (!client || event_type >= HIF_EVENT_MAX || qno >= HIF_CLIENT_QUEUES_MAX)
		return;

	if (!test_and_set_bit(qno, &client->queue_mask[event_type]))
		client->event_handler(client->priv, event_type, qno);
}

static int hif_lib_event_dummy(void *priv, int event_type, int qno)
{
	return 0;
}

/*
 * Allocate and pre-fill a client's Rx queue descriptors -- ownership of
 * every slot starts with the HIF driver (CL_DESC_OWN) so it can start
 * filling them in as soon as hif_process_client_req(REQUEST_CL_REGISTER)
 * below hands the queue addresses over.
 */
static int hif_lib_client_init_rx_buffers(struct hif_client_s *client, int q_size)
{
	struct hif_client_rx_queue *queue;
	struct rx_queue_desc *desc;
	int qno, i;

	client->rx_qbase = kcalloc(client->rx_qn * q_size, sizeof(struct rx_queue_desc), GFP_KERNEL);
	if (!client->rx_qbase)
		return -ENOMEM;

	for (qno = 0; qno < client->rx_qn; qno++) {
		queue = &client->rx_q[qno];
		queue->base = client->rx_qbase + qno * q_size * sizeof(struct rx_queue_desc);
		queue->size = q_size;
		queue->read_idx = 0;
		queue->write_idx = 0;
	}

	for (qno = 0; qno < client->rx_qn; qno++) {
		queue = &client->rx_q[qno];
		desc = queue->base;

		for (i = 0; i < queue->size; i++, desc++)
			desc->ctrl = CL_DESC_BUF_LEN(pfe_pkt_size) | CL_DESC_OWN;
	}

	return 0;
}

static void hif_lib_client_release_rx_buffers(struct hif_client_s *client)
{
	struct rx_queue_desc *desc;
	int qno, i;
	void *buf;

	for (qno = 0; qno < client->rx_qn; qno++) {
		desc = client->rx_q[qno].base;

		for (i = 0; i < client->rx_q[qno].size; i++, desc++) {
			buf = desc->data;
			if (buf)
				kfree(buf - pfe_pkt_headroom);
		}
	}

	kfree(client->rx_qbase);
}

static int hif_lib_client_init_tx_buffers(struct hif_client_s *client, int q_size)
{
	struct hif_client_tx_queue *queue;
	int qno;

	client->tx_qbase = kcalloc(client->tx_qn * q_size, sizeof(struct tx_queue_desc), GFP_KERNEL);
	if (!client->tx_qbase)
		return -ENOMEM;

	for (qno = 0; qno < client->tx_qn; qno++) {
		queue = &client->tx_q[qno];
		queue->base = client->tx_qbase + qno * q_size * sizeof(struct tx_queue_desc);
		queue->size = q_size;
		queue->read_idx = 0;
		queue->write_idx = 0;
		queue->tx_pending = 0;
	}

	return 0;
}

static void hif_lib_client_release_tx_buffers(struct hif_client_s *client)
{
	int qno;

	for (qno = 0; qno < client->tx_qn; qno++) {
		if (client->tx_q[qno].tx_pending)
			pr_err("%s: client %d queue %d has pending tx packets\n",
			       __func__, client->id, qno);
	}

	kfree(client->tx_qbase);
}

int hif_lib_client_register(struct hif_client_s *client)
{
	struct hif_client_shm *client_shm;
	struct hif_shm *hif_shm;
	int rc;

	if (!client->pfe || client->id >= HIF_CLIENTS_MAX || g_pfe->hif_client[client->id])
		return -EINVAL;

	hif_shm = client->pfe->hif.shm;

	rc = hif_lib_client_init_rx_buffers(client, client->rx_qsize);
	if (rc)
		return rc;

	rc = hif_lib_client_init_tx_buffers(client, client->tx_qsize);
	if (rc) {
		hif_lib_client_release_rx_buffers(client);
		return rc;
	}

	if (!client->event_handler)
		client->event_handler = hif_lib_event_dummy;

	client_shm = &hif_shm->client[client->id];
	client_shm->rx_qbase = (u32)client->rx_qbase;
	client_shm->rx_qsize = client->rx_qsize;
	client_shm->tx_qbase = (u32)client->tx_qbase;
	client_shm->tx_qsize = client->tx_qsize;
	client_shm->ctrl = (client->tx_qn << CLIENT_CTRL_TX_Q_CNT_OFST) |
			   (client->rx_qn << CLIENT_CTRL_RX_Q_CNT_OFST);

	memset(client->queue_mask, 0, sizeof(client->queue_mask));

	hif_process_client_req(&client->pfe->hif, REQUEST_CL_REGISTER, client->id, 0);

	g_pfe->hif_client[client->id] = client;

	return 0;
}

int hif_lib_client_unregister(struct hif_client_s *client)
{
	struct pfe *pfe = client->pfe;

	hif_process_client_req(&pfe->hif, REQUEST_CL_UNREGISTER, client->id, 0);

	hif_lib_client_release_tx_buffers(client);
	hif_lib_client_release_rx_buffers(client);

	g_pfe->hif_client[client->id] = NULL;

	return 0;
}

/*
 * Both no-ops in the vendor driver itself (TMU-level per-queue enable/
 * disable was apparently never implemented there either) -- ported as
 * the same no-ops rather than silently dropped, so pfe_eth.c's
 * open/close can call them in the right place for whichever future
 * stage might give them a real body.
 */
int hif_lib_tmu_queue_start(struct hif_client_s *client, int qno)
{
	return 0;
}

int hif_lib_tmu_queue_stop(struct hif_client_s *client, int qno)
{
	return 0;
}

/*
 * Writes the 6-byte struct hif_hdr directly into the packet buffer just
 * before the data pointer the caller passed in (hif_lib_xmit_pkt()
 * below decrements it there before calling this) -- the HIF hardware
 * ring carries just one contiguous DMA buffer per packet, header and
 * payload together, not a separate header descriptor.
 */
static inline void hif_hdr_write(struct hif_hdr *pkt_hdr, unsigned int client_id,
				  unsigned int qno, u32 client_ctrl)
{
	if (!((unsigned long)pkt_hdr & 0x3)) {
		((u32 *)pkt_hdr)[0] = (client_ctrl << 16) | (qno << 8) | client_id;
	} else {
		((u16 *)pkt_hdr)[0] = (qno << 8) | client_id;
		((u16 *)pkt_hdr)[1] = client_ctrl;
	}
}

/*
 * Queue one full, non-fragmented packet for transmission. Only the
 * vendor's HIF_FIRST_BUFFER|HIF_LAST_BUFFER|HIF_DATA_VALID single-
 * descriptor case is ported (see pfe_hif_lib.h) -- data must have at
 * least sizeof(struct hif_hdr) of headroom before it, since the header
 * is written in place there; the caller (pfe_eth.c) is responsible for
 * making sure that holds (pskb_expand_head() if not).
 */
int hif_lib_xmit_pkt(struct hif_client_s *client, unsigned int qno, void *data,
		      unsigned int len, u32 client_ctrl, void *client_data)
{
	struct hif_client_tx_queue *queue = &client->tx_q[qno];
	struct tx_queue_desc *desc = queue->base + queue->write_idx;

	if (queue->tx_pending >= queue->size)
		return 1;

	data -= sizeof(struct hif_hdr);
	len += sizeof(struct hif_hdr);

	hif_hdr_write(data, client->id, qno, client_ctrl);

	desc->data = client_data;
	desc->ctrl = CL_DESC_OWN | CL_DESC_FLAGS(HIF_FIRST_BUFFER | HIF_LAST_BUFFER | HIF_DATA_VALID);

	if (hif_xmit_pkt(&client->pfe->hif, client->id, qno, data, len))
		return 1;

	queue->write_idx = (queue->write_idx + 1) & (queue->size - 1);
	queue->tx_pending++;
	queue->jiffies_last_packet = jiffies;

	return 0;
}

/*
 * Dequeues one completed Tx packet's client_data (the skb pointer
 * hif_lib_xmit_pkt() was given), so the caller can free it. Calls
 * hif_tx_done_process() (Stage P5, pfe_hif.c) to make the HIF ring
 * itself progress -- that's also where the Tx buffer's DMA mapping
 * gets torn down.
 */
void *hif_lib_tx_get_next_complete(struct hif_client_s *client, int qno,
				    unsigned int *flags, int count)
{
	struct hif_client_tx_queue *queue = &client->tx_q[qno];
	struct tx_queue_desc *desc = queue->base + queue->read_idx;

	if (!queue->tx_pending)
		return NULL;

	if (desc->ctrl & CL_DESC_OWN) {
		hif_tx_done_process(&client->pfe->hif, count);

		if (desc->ctrl & CL_DESC_OWN)
			return NULL;
	}

	queue->read_idx = (queue->read_idx + 1) & (queue->size - 1);
	queue->tx_pending--;
	*flags = CL_DESC_GET_FLAGS(desc->ctrl);

	return desc->data;
}

void *hif_lib_receive_pkt(struct hif_client_s *client, int qno, int *len, int *ofst,
			   unsigned int *rx_ctrl, unsigned int *desc_ctrl, void **priv_data)
{
	struct hif_client_rx_queue *queue = &client->rx_q[qno];
	struct rx_queue_desc *desc = queue->base + queue->read_idx;
	void *pkt = NULL;

	if (desc->ctrl & CL_DESC_OWN)
		return NULL;

	pkt = desc->data - pfe_pkt_headroom;

	*rx_ctrl = desc->client_ctrl;
	*desc_ctrl = desc->ctrl;

	if (desc->ctrl & CL_DESC_FIRST) {
		u16 size = *rx_ctrl >> 24;

		if (size) {
			*len = CL_DESC_BUF_LEN(desc->ctrl) - PFE_PKT_HEADER_SZ - size;
			*ofst = pfe_pkt_headroom + PFE_PKT_HEADER_SZ + size;
			*priv_data = desc->data + PFE_PKT_HEADER_SZ;
		} else {
			*len = CL_DESC_BUF_LEN(desc->ctrl) - PFE_PKT_HEADER_SZ;
			*ofst = pfe_pkt_headroom + PFE_PKT_HEADER_SZ;
			*priv_data = NULL;
		}
	} else {
		*len = CL_DESC_BUF_LEN(desc->ctrl);
		*ofst = pfe_pkt_headroom;
	}

	/* Needed so a client that never consumes this slot again (e.g. on
	 * unregister) doesn't end up freeing the same buffer twice.
	 */
	desc->data = NULL;
	smp_wmb();

	desc->ctrl = CL_DESC_BUF_LEN(pfe_pkt_size) | CL_DESC_OWN;
	queue->read_idx = (queue->read_idx + 1) & (queue->size - 1);

	return pkt;
}

int hif_lib_event_handler_start(struct hif_client_s *client, int event, int qno)
{
	struct hif_client_rx_queue *queue = &client->rx_q[qno];
	struct rx_queue_desc *desc = queue->base + queue->read_idx;

	if (event >= HIF_EVENT_MAX || qno >= HIF_CLIENT_QUEUES_MAX)
		return -1;

	test_and_clear_bit(qno, &client->queue_mask[event]);

	switch (event) {
	case EVENT_RX_PKT_IND:
		/* A packet may have arrived between the caller finishing its
		 * drain and this clearing the mask -- if so, re-indicate
		 * immediately instead of risking a missed wakeup.
		 */
		if (!(desc->ctrl & CL_DESC_OWN))
			hif_lib_indicate_client(client->id, EVENT_RX_PKT_IND, qno);
		break;
	default:
		break;
	}

	return 0;
}

int pfe_hif_lib_init(struct pfe *pfe)
{
	int rc;

	pfe->hif.shm = &ghif_shm;
	rc = pfe_hif_shm_init(pfe->hif.shm);

	dev_info(pfe->dev, "pfe_hif_lib_init: pkt size %u, rx buffers %u\n",
		 pfe_pkt_size, HIF_RX_DESC_NT);

	return rc;
}

void pfe_hif_lib_exit(struct pfe *pfe)
{
	pfe_hif_shm_clean(pfe->hif.shm);
}
