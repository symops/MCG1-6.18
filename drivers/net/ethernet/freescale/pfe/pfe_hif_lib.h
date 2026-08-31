/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HIF client-facing shared-memory layout and helpers -- ported from the
 * 3.2.26 vendor tree's pfe_ctrl/pfe_hif_lib.h (kmodules/mspd-c2k/pfe/ in
 * symops/MCG1-3.2.26).
 *
 * Stage P5 added what pfe_hif.c's init/exit/ISR/NAPI path itself needs
 * (the shared-memory rx buffer pool, and the client event indication
 * used by the Rx path). Stage P7 added the client-registration API
 * (hif_lib_client_register()/unregister()) that pfe_eth.c calls from
 * its .ndo_open/.ndo_stop. Stage P8 adds the TX submission path
 * (hif_lib_xmit_pkt(), hif_lib_tx_get_next_complete()) and
 * hif_lib_event_handler_start() (the Rx re-arm half of the
 * hif_lib_indicate_client() edge-triggered wakeup below). Still out of
 * scope: TSO and TX credit/QoS accounting -- neither is needed for
 * plain, non-jumbo Ethernet traffic at the pace this baseline targets
 * (DHCP+ping), and dropping them avoids reintroducing the FCI-adjacent
 * machinery this whole port has deliberately left out since Stage P1.
 */
#ifndef _PFE_HIF_LIB_H_
#define _PFE_HIF_LIB_H_

#include "pfe_hif.h"

enum {
	REQUEST_CL_REGISTER = 0,
	REQUEST_CL_UNREGISTER,
	HIF_REQUEST_MAX
};

enum {
	EVENT_HIGH_RX_WM = 0,
	EVENT_RX_PKT_IND,
	EVENT_TXDONE_IND,
	HIF_EVENT_MAX
};

struct hif_client_rx_queue {
	struct rx_queue_desc *base;
	u32 size;
	u32 read_idx;
	u32 write_idx;
};

struct hif_client_tx_queue {
	struct tx_queue_desc *base;
	u32 size;
	u32 read_idx;
	u32 write_idx;
	u32 tx_pending;
	unsigned long jiffies_last_packet;
};

struct hif_client_s {
	int id;
	int tx_qn;
	int rx_qn;
	void *rx_qbase;
	void *tx_qbase;
	int tx_qsize;
	int rx_qsize;
	struct hif_client_tx_queue tx_q[HIF_CLIENT_QUEUES_MAX];
	struct hif_client_rx_queue rx_q[HIF_CLIENT_QUEUES_MAX];
	int (*event_handler)(void *priv, int event, int data);
	unsigned long queue_mask[HIF_EVENT_MAX];
	struct pfe *pfe;
	void *priv;
};

/* Client-specific shared memory: number of Rx/Tx queues, base addresses
 * and queue sizes.
 */
struct hif_client_shm {
	volatile u32 ctrl; /* 0-7: number of Rx queues, 8-15: number of Tx queues */
	volatile u32 rx_qbase;
	volatile u32 rx_qsize;
	volatile u32 tx_qbase;
	volatile u32 tx_qsize;
};

#define CLIENT_CTRL_RX_Q_CNT_OFST	0
#define CLIENT_CTRL_TX_Q_CNT_OFST	8
#define CLIENT_CTRL_RX_Q_CNT(ctrl)	(((ctrl) >> CLIENT_CTRL_RX_Q_CNT_OFST) & 0xFF)
#define CLIENT_CTRL_TX_Q_CNT(ctrl)	(((ctrl) >> CLIENT_CTRL_TX_Q_CNT_OFST) & 0xFF)

/* Shared memory used to communicate between the HIF driver and host/
 * client drivers. rx_buf_pool/rx_buf_pool_cnt must be filled in (by
 * pfe_hif_shm_init()) before the HIF driver starts; rx_buf_pool_cnt
 * must be >= HIF_RX_DESC_NT.
 */
struct hif_shm {
	volatile u32 rx_buf_pool_cnt;
	volatile void *rx_buf_pool[HIF_RX_DESC_NT];
	volatile u32 gClient_status;
	volatile u32 hif_qfull;
	volatile u32 hif_qresume;
	struct hif_client_shm client[HIF_CLIENTS_MAX];
};

#define CL_DESC_OWN		(1 << 31) /* Ownership: set = HIF driver owns it */
#define CL_DESC_LAST		(1 << 30)
#define CL_DESC_FIRST		(1 << 29)
#define CL_DESC_BUF_LEN(x)	((x) & 0xFFFF)
#define CL_DESC_FLAGS(x)	(((x) & 0xF) << 16)
#define CL_DESC_GET_FLAGS(x)	(((x) >> 16) & 0xF)

struct rx_queue_desc {
	void *data;
	u32 ctrl;
	u32 client_ctrl;
};

struct tx_queue_desc {
	void *data;
	u32 ctrl;
};

/*
 * "ip_header = 64 + 6(hif_header) + 14 (MAC Header)" lands 4-byte
 * aligned, which HIF Rx needs for good performance (vendor comment).
 */
#define PFE_PKT_HEADER_SZ	sizeof(struct hif_hdr)
#define PFE_BUF_SIZE		2048	/* headroom + pkt size + skb shared info */
#define PFE_PKT_HEADROOM	128
#define PFE_PKT_SIZE		1544	/* maximum ethernet packet size */

#define GFP_DMA_PFE		0

extern unsigned int pfe_pkt_size;
extern unsigned int pfe_pkt_headroom;

int pfe_hif_lib_init(struct pfe *pfe);
void pfe_hif_lib_exit(struct pfe *pfe);
void hif_lib_indicate_client(int cl_id, int event, int data);

/*
 * Client registration API (Stage P7) -- lets a net_device driver
 * (pfe_eth.c) hand the HIF driver its Rx/Tx shared-memory queues and
 * become reachable via hif_lib_indicate_client() above. The Tx
 * submission path itself (hif_lib_xmit_pkt() and friends) stays out of
 * scope until Stage P8 actually needs to send a packet.
 */
int hif_lib_client_register(struct hif_client_s *client);
int hif_lib_client_unregister(struct hif_client_s *client);
int hif_lib_tmu_queue_start(struct hif_client_s *client, int qno);
int hif_lib_tmu_queue_stop(struct hif_client_s *client, int qno);

/*
 * TX submission (Stage P8) -- hif_lib_xmit_pkt() queues one full,
 * non-fragmented packet (the only case this port implements; the
 * vendor's fragmented/TSO submission path via __hif_lib_xmit_pkt() and
 * multiple HIF_FIRST_BUFFER/HIF_LAST_BUFFER-flagged calls per packet
 * isn't ported). hif_lib_tx_get_next_complete() dequeues a completed
 * packet's client_data (the skb pointer the caller passed in) so the
 * caller can free it -- this is also how the underlying HIF Tx ring
 * descriptor's DMA mapping gets torn down, via hif_tx_done_process()
 * inside it (already ported in pfe_hif.c since Stage P5).
 */
int hif_lib_xmit_pkt(struct hif_client_s *client, unsigned int qno, void *data,
		      unsigned int len, u32 client_ctrl, void *client_data);
void *hif_lib_tx_get_next_complete(struct hif_client_s *client, int qno,
				    unsigned int *flags, int count);

/*
 * Dequeues one received packet's buffer (Stage P8). *ofst is the byte
 * offset from the start of that buffer where the actual payload begins
 * (past the hif_hdr, and past an optional firmware-prepended private
 * header whose size is packed into the top byte of *rx_ctrl -- this
 * port never sets that up on the Tx side, so it should always read 0
 * for anything this driver itself sends, but a packet ingested straight
 * off the wire goes through the same CLASS PE code path regardless, so
 * the offset math is kept general rather than assumed away). *priv_data
 * points at that private header if present, NULL otherwise -- unused by
 * this port (no consumer for it), kept only because getting *ofst right
 * depends on computing it.
 */
void *hif_lib_receive_pkt(struct hif_client_s *client, int qno, int *len, int *ofst,
			   unsigned int *rx_ctrl, unsigned int *desc_ctrl, void **priv_data);

/*
 * Rx re-arm (Stage P8) -- call after draining a client's Rx queue for
 * the given (event, qno) pair in response to hif_lib_indicate_client()
 * calling this client's event_handler. hif_lib_indicate_client() is
 * edge-triggered (queue_mask[] latches once until cleared here), so
 * skipping this call would mean this queue never gets indicated again.
 * Also re-checks for a packet that arrived during the drain (a race
 * between "queue looked empty" and "mask cleared") and re-indicates
 * immediately if so, rather than risking a missed wakeup.
 */
int hif_lib_event_handler_start(struct hif_client_s *client, int event, int qno);

#endif /* _PFE_HIF_LIB_H_ */
