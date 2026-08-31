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
