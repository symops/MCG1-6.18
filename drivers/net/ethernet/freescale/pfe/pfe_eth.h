/* SPDX-License-Identifier: GPL-2.0 */
/*
 * net_device/MDIO/PHY layer (Stage P7) and Tx/Rx traffic path (Stage
 * P8) for a single PFE GEM port -- ported from the 3.2.26 vendor tree's
 * pfe_ctrl/pfe_eth.c/pfe_eth.h (kmodules/mspd-c2k/pfe/ in
 * symops/MCG1-3.2.26), GEM0 only so far (Stage P9 adds GEM1/GEM2 by the
 * same pattern) -- see pfe_eth.c's banner comment for the full scope
 * writeup.
 *
 * struct pfe_eth_priv_s below is a trimmed version of the vendor
 * pfe_eth_priv_s: dropped are the vendor's lro/low/high per-net_device
 * NAPI instances (this port centralized Rx polling at the HIF level in
 * Stage P5 -- see pfe_hif.c's single shared struct pfe_hif.napi -- so a
 * second, per-client NAPI layer doesn't apply here the way it did to
 * the vendor's 3.2.26 architecture), the TX path's per-queue timers/
 * DMA-map bookkeeping/credit accounting (needed for TSO and multi-queue
 * QoS, neither of which this port implements), and everything tied to
 * ethtool/sysfs stats reporting that was never ported to begin with.
 */
#ifndef _PFE_ETH_H_
#define _PFE_ETH_H_

#include <linux/clk.h>
#include <linux/mii.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/spinlock.h>

#include "pfe_hif_lib.h"

#define EMAC_TXQ_CNT		16
#define EMAC_TXQ_DEPTH		HIF_TX_DESC_NT
#define EMAC_RXQ_CNT		3
#define EMAC_RXQ_DEPTH		HIF_RX_DESC_NT
#define EMAC_MDIO_TIMEOUT	1000

struct pfe_eth_priv_s {
	struct pfe *pfe;
	struct hif_client_s client;

	/* TMU Tx queue numbers this GEM's traffic is scheduled on -- only
	 * ever passed to hif_lib_tmu_queue_start()/stop() (both no-ops,
	 * see pfe_hif_lib.c), since this port's actual Tx submission
	 * (hif_lib_xmit_pkt(), Stage P8) always uses a single fixed
	 * client-level queue (PFE_ETH_TXQ in pfe_eth.c) instead of the
	 * vendor's per-packet QoS classification into one of 16.
	 */
	int low_tmuQ;
	int high_tmuQ;

	struct net_device *dev;
	struct device_node *of_node;
	unsigned int id;		/* GEM id: 0, 1 or 2 */

	spinlock_t lock;		/* protects oldspeed/oldduplex/oldlink below */

	void __iomem *EMAC_baseaddr;
	void __iomem *GPI_baseaddr;

	struct phy_device *phydev;
	phy_interface_t phy_mode;
	int oldspeed;
	int oldduplex;
	int oldlink;

	struct mii_bus *mii_bus;	/* shared across all GEMs on this PFE */
	struct clk *gemtx_clk;		/* shared across all GEMs on this PFE */
};

int pfe_eth_init(struct pfe *pfe);
void pfe_eth_exit(struct pfe *pfe);

#endif /* _PFE_ETH_H_ */
