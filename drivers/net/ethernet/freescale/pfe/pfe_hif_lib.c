// SPDX-License-Identifier: GPL-2.0
/*
 * HIF client-facing shared-memory setup and event indication -- ported
 * from the 3.2.26 vendor tree's pfe_ctrl/pfe_hif_lib.c (kmodules/
 * mspd-c2k/pfe/ in symops/MCG1-3.2.26).
 *
 * Stage P5 scope only -- see pfe_hif_lib.h for what's deliberately not
 * here yet (client registration, TX path, TX credit/QoS), all of which
 * are only reachable once a client is actually registered (Stage
 * P7-P9's pfe_eth.c).
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
