// SPDX-License-Identifier: GPL-2.0
/*
 * PFE Stage P7-P8: net_device/MDIO/PHY bring-up (P7) and a working
 * traffic path (P8) for GEM0. Ported from the 3.2.26 vendor tree's
 * pfe_ctrl/pfe_eth.c (kmodules/mspd-c2k/pfe/ in symops/MCG1-3.2.26,
 * 2846 lines) -- almost none of that file's size is ported here; see
 * pfe_eth.h's banner for what was structurally dropped.
 *
 * Stage P7 confirmed on real hardware: PHY address 0 (Broadcom
 * BCM54612E), MDIO bus and PHY connect both working (see
 * Documentation/arm/ls1024a-wdmycloud.rst). Getting there needed one
 * fix not obvious from the vendor source: each GEM has its own
 * "extphy" reference clock (LS1024A_CLK_EXTPHY0/1/2), separate from
 * the pfe node's shared "gemtx", that this port never requested --
 * without it, the kernel's own "disabling unused clocks" late-boot
 * step gates it off a couple seconds in, and every MDIO transaction
 * after that silently returns stale data instead of erroring (see
 * pfe_eth_probe_gem()).
 *
 * Stage P8 adds the actual Tx/Rx path:
 *
 *  - pfe_eth_send_packet() (.ndo_start_xmit) submits via
 *    hif_lib_xmit_pkt() (Stage P8, pfe_hif_lib.c) -- one full,
 *    non-fragmented packet per call, no TSO, no QoS classification
 *    (everything goes on a single fixed queue, PFE_ETH_TXQ). No stop/
 *    wake-queue flow control either: the 1024-deep ring (EMAC_TXQ_DEPTH)
 *    is comfortably more than this baseline's DHCP+ping target needs,
 *    so a full ring just drops the packet instead.
 *  - pfe_eth_rx_drain() consumes what hif_lib_client_register() (Stage
 *    P7) made this GEM reachable for, called directly from
 *    pfe_eth_event_handler() -- itself called synchronously from
 *    hif_lib_indicate_client() (Stage P5), which already runs inside
 *    the shared HIF-level NAPI poll (pfe_hif.c's struct pfe_hif.napi).
 *    No second NAPI layer needed: unlike the vendor's one-NAPI-triple-
 *    per-GEM design, this port centralized Rx polling at the HIF level
 *    back in Stage P5 (a single physical Rx DMA ring shared by every
 *    client, demultiplexed by client id), and the drain runs to
 *    completion inline, in the same softirq context the outer HIF poll
 *    is already in -- there's nothing to schedule.
 *  - No multi-descriptor (jumbo/LRO) Rx reassembly: PFE_PKT_SIZE (1544)
 *    covers any standard MTU + headers in one HIF descriptor, so this
 *    should never actually come up; pfe_eth_rx_drain() drops
 *    defensively rather than mishandle it if it ever does.
 */

#include <linux/clk.h>
#include <linux/ethtool.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/mii.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/unaligned.h>

#include "pfe_eth.h"
#include "pfe_hw_lib.h"
#include "pfe_mod.h"

/* TMU queue numbers this GEM's traffic is scheduled on: TMU queues 0-5
 * are reserved for other PFE-internal traffic classes in the vendor
 * design; each GEM gets a low/high priority queue pair starting at 6.
 */
#define HIF_GEMAC_TMUQ_BASE	6

/*
 * This SoC has exactly one PFE, and the MDIO management registers are
 * physically wired through GEM0/EMAC1's own register block regardless
 * of which GEM's PHY is being addressed (matches the vendor board file,
 * which only ever defines MDIO bus 0). Kept as file-static state for
 * the same reason g_pfe is (see pfe_mod.h) -- there's only ever one of
 * these on real hardware, and threading a pointer to it through every
 * per-GEM probe call for no benefit isn't worth it.
 */
struct pfe_mdio_priv {
	void __iomem *EMAC_baseaddr;
	int mdc_div;
};

static struct pfe_mdio_priv pfe_mdio_priv;
static struct mii_bus *pfe_mii_bus;
static struct net_device *pfe_gem_netdevs[3];

static int pfe_mdio_wait_idle(void __iomem *emac_base)
{
	int timeout = EMAC_MDIO_TIMEOUT;

	while (!(readl(emac_base + EMAC_NETWORK_STATUS) & EMAC_PHY_IDLE)) {
		if (--timeout <= 0)
			return -ETIMEDOUT;
		udelay(10);
	}

	return 0;
}

static int pfe_eth_mdio_read(struct mii_bus *bus, int mii_id, int regnum)
{
	struct pfe_mdio_priv *mpriv = bus->priv;
	u32 write_data = 0x60020000 | (mii_id << 23) | (regnum << 18);

	writel(write_data, mpriv->EMAC_baseaddr + EMAC_PHY_MANAGEMENT);

	if (pfe_mdio_wait_idle(mpriv->EMAC_baseaddr))
		return -ETIMEDOUT;

	return readl(mpriv->EMAC_baseaddr + EMAC_PHY_MANAGEMENT) & 0xFFFF;
}

static int pfe_eth_mdio_write(struct mii_bus *bus, int mii_id, int regnum, u16 value)
{
	struct pfe_mdio_priv *mpriv = bus->priv;
	u32 write_data = 0x50020000 | (mii_id << 23) | (regnum << 18) | value;

	writel(write_data, mpriv->EMAC_baseaddr + EMAC_PHY_MANAGEMENT);

	return pfe_mdio_wait_idle(mpriv->EMAC_baseaddr);
}

static int pfe_eth_mdio_reset(struct mii_bus *bus)
{
	struct pfe_mdio_priv *mpriv = bus->priv;

	gemac_set_mdc_div(mpriv->EMAC_baseaddr, mpriv->mdc_div);

	writel(readl(mpriv->EMAC_baseaddr + EMAC_NETWORK_CONTROL) | EMAC_MDIO_EN,
	       mpriv->EMAC_baseaddr + EMAC_NETWORK_CONTROL);

	return pfe_mdio_wait_idle(mpriv->EMAC_baseaddr);
}

static int pfe_eth_mdio_init(struct pfe *pfe, struct device_node *np)
{
	struct mii_bus *bus;
	int rc, addr;

	pfe_mdio_priv.EMAC_baseaddr = EMAC1_BASE_ADDR;
	/*
	 * 96 is the only real data point found for this divisor: the GPL
	 * source drop's arch/arm/mach-comcerto/board-c2kevm.c has this
	 * exact value hand-edited into comcerto_mdio_pdata[0], alongside a
	 * comment identifying a Broadcom PHY on this bus (see the P7
	 * research writeup in the commit log) -- unconfirmed for this
	 * specific board like the PHY address itself, but a real divisor
	 * setting a wide margin under any PHY's max MDC frequency either
	 * way, so it's a safe default regardless.
	 */
	pfe_mdio_priv.mdc_div = 96;

	bus = mdiobus_alloc();
	if (!bus)
		return -ENOMEM;

	bus->name = "ls1024a-pfe-mdio";
	bus->read = pfe_eth_mdio_read;
	bus->write = pfe_eth_mdio_write;
	bus->reset = pfe_eth_mdio_reset;
	bus->priv = &pfe_mdio_priv;
	bus->phy_mask = 0; /* scan every address -- see banner comment */
	bus->parent = pfe->dev;
	snprintf(bus->id, MII_BUS_ID_SIZE, "ls1024a-pfe-mdio");

	rc = of_mdiobus_register(bus, np);
	if (rc) {
		dev_err(pfe->dev, "of_mdiobus_register() failed: %d\n", rc);
		mdiobus_free(bus);
		return rc;
	}

	for (addr = 0; addr < PHY_MAX_ADDR; addr++) {
		int id1 = mdiobus_read(bus, addr, MII_PHYSID1);
		int id2 = mdiobus_read(bus, addr, MII_PHYSID2);

		if (id1 < 0 || id2 < 0)
			continue;
		if ((id1 == 0xffff && id2 == 0xffff) || (id1 == 0 && id2 == 0))
			continue;

		dev_info(pfe->dev, "mdio: PHY responding at address %d (id %04x:%04x)\n",
			 addr, id1, id2);
	}

	pfe_mii_bus = bus;

	return 0;
}

static void pfe_eth_mdio_exit(void)
{
	if (!pfe_mii_bus)
		return;

	mdiobus_unregister(pfe_mii_bus);
	mdiobus_free(pfe_mii_bus);
	pfe_mii_bus = NULL;
}

static void pfe_gemac_init(struct pfe_eth_priv_s *priv)
{
	GEMAC_CFG cfg;

	switch (priv->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		cfg.mode = RGMII;
		break;
	case PHY_INTERFACE_MODE_GMII:
		cfg.mode = GMII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		cfg.mode = RMII;
		break;
	case PHY_INTERFACE_MODE_SGMII:
		cfg.mode = SGMII;
		break;
	case PHY_INTERFACE_MODE_MII:
	default:
		cfg.mode = MII;
		break;
	}

	/* Software defaults until adjust_link() corrects them post-autoneg. */
	cfg.speed = SPEED_1000M;
	cfg.duplex = DUPLEX_FULL;

	gemac_set_config(priv->EMAC_baseaddr, &cfg);
	gemac_allow_broadcast(priv->EMAC_baseaddr);
	gemac_disable_unicast(priv->EMAC_baseaddr);
	gemac_disable_multicast(priv->EMAC_baseaddr);
	gemac_disable_fcs_rx(priv->EMAC_baseaddr);
	gemac_enable_1536_rx(priv->EMAC_baseaddr);
	gemac_set_bus_width(priv->EMAC_baseaddr, 64);
	/* No NETIF_F_RXCSUM feature bit declared yet -- see pfe_hw_lib.c. */
	gemac_disable_rx_checksum_offload(priv->EMAC_baseaddr);
}

static void pfe_eth_adjust_link(struct net_device *dev)
{
	struct pfe_eth_priv_s *priv = netdev_priv(dev);
	struct phy_device *phydev = priv->phydev;
	unsigned long flags;
	int new_state = 0;

	spin_lock_irqsave(&priv->lock, flags);

	if (phydev->link) {
		if (phydev->duplex != priv->oldduplex) {
			gemac_set_duplex(priv->EMAC_baseaddr, phydev->duplex);
			priv->oldduplex = phydev->duplex;
			new_state = 1;
		}

		if (phydev->speed != priv->oldspeed) {
			MAC_SPEED mac_speed;

			switch (phydev->speed) {
			case SPEED_10:
				mac_speed = SPEED_10M;
				break;
			case SPEED_100:
				mac_speed = SPEED_100M;
				break;
			case SPEED_1000:
			default:
				mac_speed = SPEED_1000M;
				break;
			}
			gemac_set_speed(priv->EMAC_baseaddr, mac_speed);
			priv->oldspeed = phydev->speed;
			new_state = 1;
		}

		if (!priv->oldlink) {
			priv->oldlink = 1;
			new_state = 1;
		}
	} else if (priv->oldlink) {
		priv->oldlink = 0;
		priv->oldspeed = 0;
		priv->oldduplex = -1;
		new_state = 1;
	}

	if (new_state)
		phy_print_status(phydev);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int pfe_phy_init(struct net_device *dev)
{
	struct pfe_eth_priv_s *priv = netdev_priv(dev);
	struct phy_device *phydev;

	priv->oldlink = 0;
	priv->oldspeed = 0;
	priv->oldduplex = -1;

	phydev = of_phy_get_and_connect(dev, priv->of_node, pfe_eth_adjust_link);
	if (!phydev) {
		netdev_err(dev, "of_phy_get_and_connect() failed\n");
		return -ENODEV;
	}

	priv->phydev = phydev;
	phy_attached_info(phydev);

	return 0;
}

static void pfe_phy_exit(struct net_device *dev)
{
	struct pfe_eth_priv_s *priv = netdev_priv(dev);

	phy_disconnect(priv->phydev);
	priv->phydev = NULL;
}

static void pfe_eth_start(struct pfe_eth_priv_s *priv)
{
	if (priv->phydev)
		phy_start(priv->phydev);

	hif_lib_tmu_queue_start(&priv->client, priv->low_tmuQ);
	hif_lib_tmu_queue_start(&priv->client, priv->high_tmuQ);

	gpi_enable(priv->GPI_baseaddr);
	gemac_enable(priv->EMAC_baseaddr);
}

static void pfe_eth_stop(struct pfe_eth_priv_s *priv)
{
	gemac_disable(priv->EMAC_baseaddr);
	gpi_disable(priv->GPI_baseaddr);

	hif_lib_tmu_queue_stop(&priv->client, priv->low_tmuQ);
	hif_lib_tmu_queue_stop(&priv->client, priv->high_tmuQ);

	if (priv->phydev)
		phy_stop(priv->phydev);
}

/*
 * TX queue index this port always submits to. The client registers
 * EMAC_TXQ_CNT (16) SW queues (matching the vendor's per-packet
 * priority classification, pfe_eth_get_queuenum() in the vendor
 * driver), but this port doesn't implement any QoS classification --
 * everything goes on queue 0, the same "unclassified default" queue
 * traffic lands on in the vendor driver's own default configuration
 * (priv->default_priority, never set to anything else there either).
 */
#define PFE_ETH_TXQ	0

/*
 * Per-call cap on how many completed Tx descriptors hif_tx_done_process()
 * (called inside hif_lib_tx_get_next_complete(), Stage P5) will process
 * in one go -- distinct from pfe_hif.c's own private TX_FREE_MAX_COUNT
 * (64), which bounds the same call from the HIF ring's own housekeeping
 * path instead. 16 matches the vendor's TX_FREE_MAX_COUNT for this
 * client-level flush.
 */
#define PFE_ETH_TX_FREE_MAX	16

/*
 * Drain everything currently queued for this client/qno, copying each
 * buffer's payload into a freshly allocated skb rather than wrapping
 * the buffer itself with build_skb(). The buffer is dma_alloc_
 * coherent() memory (pfe_hif_lib.h's pfe_hif_spare_buf_get(), see the
 * comment there for why) and must go back to pfe_hif_spare_buf_put(),
 * never generic kfree() -- which rules out build_skb()/zero-copy, since
 * the network stack tears an skb's data down with kfree() once
 * head_frag is unset. This matches the vendor driver's own
 * pfe_eth_rx_skb(), which copies out of its (also specially-allocated,
 * GFP_DMA_NCNB) buffer pool for the same reason.
 *
 * Every normal Ethernet frame fits in one HIF descriptor (PFE_PKT_SIZE,
 * 1544, comfortably covers any standard MTU + headers), so the
 * multi-descriptor/jumbo reassembly the vendor's pfe_eth_rx_skb() does
 * (chaining fragments via skb_shinfo()->frag_list, tracked per-qno in
 * priv->skb_inflight[]) isn't ported -- if CL_DESC_FIRST and
 * CL_DESC_LAST aren't both set on the same descriptor, this port has
 * no way to reassemble it, so it's dropped instead of mishandled.
 */
static void pfe_eth_rx_drain(struct pfe_eth_priv_s *priv, unsigned int qno)
{
	struct net_device *dev = priv->dev;
	unsigned int rx_ctrl, desc_ctrl;
	void *buf_addr, *priv_data;
	int length, offset;
	struct sk_buff *skb;

	for (;;) {
		buf_addr = hif_lib_receive_pkt(&priv->client, qno, &length, &offset,
						&rx_ctrl, &desc_ctrl, &priv_data);
		if (!buf_addr)
			break;

		/*
		 * hif_lib_receive_pkt() hands back the true base of the
		 * dma_alloc_coherent() block (what build_skb() used to
		 * need); pfe_hif_spare_buf_put() takes the same "usable"
		 * (base + headroom) pointer pfe_hif_spare_buf_get() returns,
		 * so every free below adds the headroom back.
		 */
		if (!(desc_ctrl & CL_DESC_FIRST) || !(desc_ctrl & CL_DESC_LAST)) {
			net_warn_ratelimited("%s: dropping unsupported multi-descriptor packet (ctrl=%#x)\n",
					      dev->name, desc_ctrl);
			pfe_hif_spare_buf_put(buf_addr + PFE_PKT_HEADROOM);
			dev->stats.rx_dropped++;
			continue;
		}

		skb = netdev_alloc_skb_ip_align(dev, length);
		if (!skb) {
			pfe_hif_spare_buf_put(buf_addr + PFE_PKT_HEADROOM);
			dev->stats.rx_dropped++;
			continue;
		}

		skb_put_data(skb, buf_addr + offset, length);
		pfe_hif_spare_buf_put(buf_addr + PFE_PKT_HEADROOM);

		skb->dev = dev;
		skb->protocol = eth_type_trans(skb, dev);
		skb_checksum_none_assert(skb);

		dev->stats.rx_packets++;
		dev->stats.rx_bytes += length;

		netif_receive_skb(skb);
	}
}

static int pfe_eth_event_handler(void *data, int event, int qno)
{
	struct pfe_eth_priv_s *priv = data;

	switch (event) {
	case EVENT_RX_PKT_IND:
		pfe_eth_rx_drain(priv, qno);
		/* Edge-triggered -- re-arm so the next arrival indicates
		 * again (see hif_lib_event_handler_start()'s own comment).
		 */
		hif_lib_event_handler_start(&priv->client, EVENT_RX_PKT_IND, qno);
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Free skbs for Tx descriptors the hardware has finished with. Called
 * both opportunistically after every send (so the ring doesn't fill up
 * under sustained traffic) and forced on close() (so nothing referenced
 * by the about-to-be-freed tx_qbase leaks -- see pfe_eth_close()).
 */
static void pfe_eth_flush_txq(struct pfe_eth_priv_s *priv, int qno, int count)
{
	struct sk_buff *skb;
	unsigned int flags;

	while (count-- > 0) {
		skb = hif_lib_tx_get_next_complete(&priv->client, qno, &flags, PFE_ETH_TX_FREE_MAX);
		if (!skb)
			break;

		if (flags & HIF_DATA_VALID)
			dev_kfree_skb_any(skb);
	}
}

static int pfe_eth_open(struct net_device *dev)
{
	struct pfe_eth_priv_s *priv = netdev_priv(dev);
	struct hif_client_s *client = &priv->client;
	MAC_ADDR addr;
	int rc;

	memset(client, 0, sizeof(*client));
	client->id = PFE_CL_GEM0 + priv->id;
	client->tx_qn = EMAC_TXQ_CNT;
	client->rx_qn = EMAC_RXQ_CNT;
	client->tx_qsize = EMAC_TXQ_DEPTH;
	client->rx_qsize = EMAC_RXQ_DEPTH;
	client->priv = priv;
	client->pfe = priv->pfe;
	client->event_handler = pfe_eth_event_handler;

	rc = hif_lib_client_register(client);
	if (rc) {
		netdev_err(dev, "hif_lib_client_register(%d) failed: %d\n", client->id, rc);
		return rc;
	}

	pfe_gemac_init(priv);

	if (!is_valid_ether_addr(dev->dev_addr)) {
		netdev_err(dev, "invalid MAC address\n");
		rc = -EADDRNOTAVAIL;
		goto err_addr;
	}

	/*
	 * gemac_set_laddrN() takes a MAC_ADDR {u32 bottom; u32 top;} pair.
	 * The vendor driver gets there via (MAC_ADDR *)dev->dev_addr, which
	 * reads 2 bytes past the end of the 6-byte dev_addr[] array -- build
	 * the pair explicitly instead of copying that over-read.
	 */
	addr.bottom = get_unaligned_le32(dev->dev_addr);
	addr.top = get_unaligned_le16(dev->dev_addr + 4);
	gemac_set_laddrN(priv->EMAC_baseaddr, &addr, 1);

	if (of_property_present(priv->of_node, "phy-handle")) {
		rc = pfe_phy_init(dev);
		if (rc) {
			netdev_err(dev, "pfe_phy_init() failed: %d\n", rc);
			goto err_addr;
		}
	} else {
		netdev_info(dev, "no phy-handle in DT yet -- link will stay down\n");
	}

	pfe_eth_start(priv);
	netif_start_queue(dev);

	return 0;

err_addr:
	hif_lib_client_unregister(client);
	return rc;
}

static int pfe_eth_close(struct net_device *dev)
{
	struct pfe_eth_priv_s *priv = netdev_priv(dev);

	netif_stop_queue(dev);

	pfe_eth_stop(priv);

	if (priv->phydev)
		pfe_phy_exit(dev);

	/*
	 * Force-flush every Tx descriptor still in flight before releasing
	 * tx_qbase (inside hif_lib_client_unregister()) -- otherwise any
	 * skb pointer sitting in a not-yet-completed descriptor leaks,
	 * since the memory holding that pointer is about to be freed out
	 * from under it. EMAC_TXQ_DEPTH (1024) bounds the loop.
	 */
	pfe_eth_flush_txq(priv, PFE_ETH_TXQ, EMAC_TXQ_DEPTH);

	hif_lib_client_unregister(&priv->client);

	return 0;
}

static netdev_tx_t pfe_eth_send_packet(struct sk_buff *skb, struct net_device *dev)
{
	struct pfe_eth_priv_s *priv = netdev_priv(dev);

	if (skb_headroom(skb) < sizeof(struct hif_hdr) &&
	    pskb_expand_head(skb, sizeof(struct hif_hdr), 0, GFP_ATOMIC)) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	if (skb_linearize(skb)) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	/*
	 * No external tx_lock here: hif_lib_xmit_pkt() calls hif_xmit_pkt()
	 * (pfe_hif.c, Stage P5), which already takes hif->tx_lock itself
	 * (and already calls hif_tx_dma_start() on the success path) --
	 * wrapping this call in another hif_tx_lock()/hif_tx_unlock() pair
	 * self-deadlocked on real hardware the first time real traffic (an
	 * IPv6 MLD report) actually reached this path, since spin_lock_bh()
	 * isn't reentrant.
	 *
	 * No hardware checksum offload wired up (NETIF_F_IP_CSUM isn't a
	 * declared feature, so skb->ip_summed should never actually be
	 * CHECKSUM_PARTIAL here) and no queue-full backpressure -- ring is
	 * 1024 deep (EMAC_TXQ_DEPTH), comfortably more than this baseline's
	 * DHCP+ping target needs; a full ring just drops, like any driver
	 * without a stop/wake-queue implementation should, rather than
	 * returning NETDEV_TX_BUSY with nothing to ever un-stick it.
	 */
	if (hif_lib_xmit_pkt(&priv->client, PFE_ETH_TXQ, skb->data, skb->len, 0, skb)) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	pfe_eth_flush_txq(priv, PFE_ETH_TXQ, PFE_ETH_TX_FREE_MAX);

	return NETDEV_TX_OK;
}

static const struct net_device_ops pfe_netdev_ops = {
	.ndo_open = pfe_eth_open,
	.ndo_stop = pfe_eth_close,
	.ndo_start_xmit = pfe_eth_send_packet,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	/* SIOCGMIIPHY/SIOCGMIIREG/SIOCSMIIREG -- phy_do_ioctl() dispatches
	 * to phy_mii_ioctl() when a phydev is attached. Wired up for raw
	 * MII register access (mii-tool and similar), not otherwise used
	 * by this driver.
	 */
	.ndo_eth_ioctl = phy_do_ioctl,
};

/*
 * Without this, `ethtool eth0` (link/speed/duplex/autoneg query) fails
 * outright with "No data available" instead of reporting phydev's real
 * state -- there's no core-kernel fallback for the settings ioctl the
 * way there is for driver-info (which is why `ethtool -i eth0` worked
 * even before this was added). phy_ethtool_get/set_link_ksettings() are
 * the standard phylib-backed helpers used by most mainline drivers with
 * an attached PHY; .get_link falls back to netif_carrier_ok() when
 * there's no phydev.
 */
static const struct ethtool_ops pfe_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
};

static void pfe_eth_extphy_clk_release(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);
	clk_put(clk);
}

static int pfe_eth_probe_gem(struct pfe *pfe, struct device_node *np)
{
	struct pfe_eth_priv_s *priv;
	struct net_device *dev;
	struct clk *extphy_clk;
	u32 id;
	int rc;

	if (of_property_read_u32(np, "reg", &id) || id > 2) {
		dev_err(pfe->dev, "%pOF: missing/invalid reg property\n", np);
		return -EINVAL;
	}

	/*
	 * Each GEM has its own "extphy" reference clock line
	 * (LS1024A_CLK_EXTPHY0/1/2 in clk-ls1024a.c) distinct from the pfe
	 * node's shared "gemtx" -- confirmed on real hardware to be required
	 * for this GEM's PHY management (MDIO) to keep working at all. Without
	 * it, MDIO reads/writes still complete (the bus protocol handshake
	 * itself doesn't need it) but every read past the first ~1-2s of
	 * boot silently returns the last successfully-read value instead of
	 * the register actually requested, regardless of which PHY/register
	 * is addressed -- traced to the kernel's own "clk: Disabling unused
	 * clocks" late boot step physically gating this line off, since
	 * nothing had ever requested it before. gem0 isn't its own struct
	 * device (no of_platform_populate() for pfe's child nodes), so
	 * of_clk_get_by_name() rather than devm_clk_get() -- cleanup is tied
	 * to the parent pfe device's lifetime via devm_add_action_or_reset()
	 * instead, since nothing currently unbinds a single GEM's node
	 * independently of the whole pfe device.
	 */
	extphy_clk = of_clk_get_by_name(np, "extphy");
	if (IS_ERR(extphy_clk)) {
		dev_warn(pfe->dev, "%pOF: no extphy clock (rc=%ld) -- MDIO may stop working after boot\n",
			 np, PTR_ERR(extphy_clk));
	} else {
		rc = clk_prepare_enable(extphy_clk);
		if (rc) {
			dev_warn(pfe->dev, "%pOF: failed to enable extphy clock: %d\n", np, rc);
			clk_put(extphy_clk);
		} else {
			rc = devm_add_action_or_reset(pfe->dev, pfe_eth_extphy_clk_release, extphy_clk);
			if (rc) {
				clk_disable_unprepare(extphy_clk);
				clk_put(extphy_clk);
				return rc;
			}
		}
	}

	if (pfe_gem_netdevs[id]) {
		dev_err(pfe->dev, "%pOF: gem%u already registered\n", np, id);
		return -EEXIST;
	}

	dev = alloc_etherdev(sizeof(*priv));
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, pfe->dev);
	dev->netdev_ops = &pfe_netdev_ops;
	dev->ethtool_ops = &pfe_ethtool_ops;
	/* hif_lib_xmit_pkt() writes struct hif_hdr in place just before
	 * skb->data -- ask the stack to reserve room for it up front so
	 * pfe_eth_send_packet()'s pskb_expand_head() fallback is only ever
	 * needed for an skb that came from somewhere unusual.
	 */
	dev->needed_headroom = sizeof(struct hif_hdr);

	priv = netdev_priv(dev);
	priv->pfe = pfe;
	priv->dev = dev;
	priv->id = id;
	priv->of_node = np;
	priv->mii_bus = pfe_mii_bus;
	spin_lock_init(&priv->lock);

	switch (id) {
	case 0:
		priv->EMAC_baseaddr = EMAC1_BASE_ADDR;
		priv->GPI_baseaddr = EGPI1_BASE_ADDR;
		break;
	case 1:
		priv->EMAC_baseaddr = EMAC2_BASE_ADDR;
		priv->GPI_baseaddr = EGPI2_BASE_ADDR;
		break;
	case 2:
		priv->EMAC_baseaddr = EMAC3_BASE_ADDR;
		priv->GPI_baseaddr = EGPI3_BASE_ADDR;
		break;
	}

	priv->low_tmuQ = HIF_GEMAC_TMUQ_BASE + id * 2;
	priv->high_tmuQ = priv->low_tmuQ + 1;

	if (of_get_phy_mode(np, &priv->phy_mode))
		priv->phy_mode = PHY_INTERFACE_MODE_MII;

	if (of_get_ethdev_address(np, dev))
		eth_hw_addr_random(dev);

	rc = register_netdev(dev);
	if (rc) {
		dev_err(pfe->dev, "register_netdev() failed for gem%u: %d\n", id, rc);
		free_netdev(dev);
		return rc;
	}

	dev_info(pfe->dev, "gem%u registered as %s (phy-mode %s)\n",
		 id, dev->name, phy_modes(priv->phy_mode));

	pfe_gem_netdevs[id] = dev;

	return 0;
}

int pfe_eth_init(struct pfe *pfe)
{
	struct device_node *pfe_np = pfe->dev->of_node;
	struct device_node *mdio_np;
	struct device_node *np;
	int rc;

	/*
	 * Enabled for the platform device's whole lifetime, not scoped to
	 * .ndo_open/.ndo_stop like the vendor driver's own clk_enable()/
	 * clk_disable() calls in pfe_eth_open()/pfe_eth_close() -- a plain
	 * devm_clk_get() here left the clock unprepared/disabled between
	 * probe (~1.2s) and the first .ndo_open() (~10s, whenever ifup
	 * actually runs), and the kernel's own late_initcall "clk: Disabling
	 * unused clocks" (~2.2s) physically gates off exactly this kind of
	 * still-unused clock in that window. Confirmed on real hardware:
	 * every MDIO transaction after that gate-then-open cycle returned
	 * the same stuck stale value regardless of which register was
	 * requested, as if the GEMAC's MDIO management sub-block never
	 * recovered from losing (and later regaining) its clock mid-boot.
	 */
	pfe->clk_gem_tx = devm_clk_get_enabled(pfe->dev, "gemtx");
	if (IS_ERR(pfe->clk_gem_tx))
		return dev_err_probe(pfe->dev, PTR_ERR(pfe->clk_gem_tx),
				      "failed to get/enable gemtx clock\n");

	mdio_np = of_get_child_by_name(pfe_np, "mdio");
	if (!mdio_np) {
		dev_info(pfe->dev, "no mdio child node -- skipping ethernet bring-up\n");
		return 0;
	}

	rc = pfe_eth_mdio_init(pfe, mdio_np);
	of_node_put(mdio_np);
	if (rc)
		return rc;

	for_each_available_child_of_node(pfe_np, np) {
		if (!of_device_is_compatible(np, "fsl,ls1024a-pfe-gem"))
			continue;

		rc = pfe_eth_probe_gem(pfe, np);
		if (rc)
			dev_err(pfe->dev, "failed to probe %pOF: %d\n", np, rc);
	}

	return 0;
}

void pfe_eth_exit(struct pfe *pfe)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (!pfe_gem_netdevs[i])
			continue;

		unregister_netdev(pfe_gem_netdevs[i]);
		free_netdev(pfe_gem_netdevs[i]);
		pfe_gem_netdevs[i] = NULL;
	}

	pfe_eth_mdio_exit();
}
