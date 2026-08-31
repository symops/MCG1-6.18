// SPDX-License-Identifier: GPL-2.0
/*
 * PFE Stage P7: net_device/MDIO/PHY bring-up for GEM0. Ported from the
 * 3.2.26 vendor tree's pfe_ctrl/pfe_eth.c (kmodules/mspd-c2k/pfe/ in
 * symops/MCG1-3.2.26, 2846 lines) -- almost none of that file's size is
 * ported here; see pfe_eth.h's banner for what was structurally dropped.
 *
 * Deliberately NOT in this stage:
 *
 *  - .ndo_start_xmit doesn't submit real packets yet (Stage P8) -- it
 *    exists only because net_device_ops requires a working xmit the
 *    moment the interface is IFF_UP (e.g. an outgoing ARP the instant
 *    link comes up would otherwise call a NULL function pointer), so
 *    it silently drops.
 *  - No per-GEM NAPI: this port's HIF layer (Stage P5) already centralized
 *    Rx polling in a single shared NAPI instance (pfe_hif.c's
 *    struct pfe_hif.napi), unlike the vendor's one-NAPI-triple-per-GEM
 *    design. hif_lib_client_register() (Stage P7, in pfe_hif_lib.c) is
 *    what makes this GEM reachable from that existing Rx path; consuming
 *    what lands in its client Rx queue into real skbs is Stage P8's job.
 *  - PHY address is unconfirmed for this exact board (see
 *    Documentation/arm/ls1024a-wdmycloud.rst and the porting plan's own
 *    "open risk #1"). Rather than guess, the MDIO bus is registered with
 *    a full address scan (phy_mask = 0) and every responding address is
 *    logged; the DTS deliberately has no phy-handle yet, so PHY connect
 *    is skipped and link simply stays down for this first hardware
 *    round-trip. Once a real address is confirmed, a phy-handle can be
 *    added to the DTS with no code change needed here.
 */

#include <linux/clk.h>
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

	rc = hif_lib_client_register(client);
	if (rc) {
		netdev_err(dev, "hif_lib_client_register(%d) failed: %d\n", client->id, rc);
		return rc;
	}

	rc = clk_prepare_enable(priv->gemtx_clk);
	if (rc) {
		netdev_err(dev, "failed to enable gemtx clock: %d\n", rc);
		goto err_clk;
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
	clk_disable_unprepare(priv->gemtx_clk);
err_clk:
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

	clk_disable_unprepare(priv->gemtx_clk);

	hif_lib_client_unregister(&priv->client);

	return 0;
}

static netdev_tx_t pfe_eth_send_packet(struct sk_buff *skb, struct net_device *dev)
{
	/* Stage P8 adds the real HIF Tx submission path -- see this file's
	 * banner comment for why this can't just be left unset.
	 */
	dev_kfree_skb(skb);
	dev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static const struct net_device_ops pfe_netdev_ops = {
	.ndo_open = pfe_eth_open,
	.ndo_stop = pfe_eth_close,
	.ndo_start_xmit = pfe_eth_send_packet,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int pfe_eth_probe_gem(struct pfe *pfe, struct device_node *np)
{
	struct pfe_eth_priv_s *priv;
	struct net_device *dev;
	u32 id;
	int rc;

	if (of_property_read_u32(np, "reg", &id) || id > 2) {
		dev_err(pfe->dev, "%pOF: missing/invalid reg property\n", np);
		return -EINVAL;
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

	priv = netdev_priv(dev);
	priv->pfe = pfe;
	priv->dev = dev;
	priv->id = id;
	priv->of_node = np;
	priv->mii_bus = pfe_mii_bus;
	priv->gemtx_clk = pfe->clk_gem_tx;
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

	pfe->clk_gem_tx = devm_clk_get(pfe->dev, "gemtx");
	if (IS_ERR(pfe->clk_gem_tx))
		return dev_err_probe(pfe->dev, PTR_ERR(pfe->clk_gem_tx),
				      "failed to get gemtx clock\n");

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
