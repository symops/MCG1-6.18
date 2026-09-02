/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PFE_MOD_H_
#define _PFE_MOD_H_

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/reset.h>
#include <linux/sizes.h>

#include "pfe_cbus.h"
#include "pfe_ctrl.h"
#include "pfe_hif.h"

/* Client library's own struct (pfe_hif_lib.h, Stage P7-P9); only ever
 * referenced here as a pointer, so a forward declaration avoids a
 * circular include (that header embeds a "struct pfe *pfe" pointer
 * right back).
 */
struct hif_client_s;

/*
 * DDR carve-out sub-layout, ported from the 3.2.26 vendor tree's
 * pfe_ctrl/pfe_mod.h (kmodules/mspd-c2k/pfe/ in symops/MCG1-3.2.26),
 * keeping only the non-PCI (CONFIG_PLATFORM_PCI unset) branch -- this
 * board's PFE hangs off the AXI/cbus, not PCI (see pfe_pci.c in the
 * vendor tree, not ported here at all). BMU2_BUF_COUNT's value is
 * chosen so this sub-layout totals 12MiB, matching the "ddr" reserved-
 * memory region size in the board DTS -- both were derived
 * independently (this from the vendor header, that from
 * COMCERTO_PFE_DDR_SIZE in the vendor board file) and agree.
 */
#define ROUTE_TABLE_BASEADDR	0
#define ROUTE_TABLE_HASH_BITS	15	/* 32K entries */
#define ROUTE_TABLE_SIZE	((1 << ROUTE_TABLE_HASH_BITS) * CLASS_ROUTE_SIZE)
#define BMU2_DDR_BASEADDR	(ROUTE_TABLE_BASEADDR + ROUTE_TABLE_SIZE)
#define BMU2_BUF_COUNT		(4096 - 256)	/* total DDR size of 12MiB */
#define BMU2_DDR_SIZE		(DDR_BUF_SIZE * BMU2_BUF_COUNT)
#define UTIL_CODE_BASEADDR	(BMU2_DDR_BASEADDR + BMU2_DDR_SIZE)
#define UTIL_CODE_SIZE		(128 * SZ_1K)
#define UTIL_DDR_DATA_BASEADDR	(UTIL_CODE_BASEADDR + UTIL_CODE_SIZE)
#define UTIL_DDR_DATA_SIZE	(64 * SZ_1K)
#define CLASS_DDR_DATA_BASEADDR (UTIL_DDR_DATA_BASEADDR + UTIL_DDR_DATA_SIZE)
#define CLASS_DDR_DATA_SIZE	(32 * SZ_1K)
#define TMU_DDR_DATA_BASEADDR	(CLASS_DDR_DATA_BASEADDR + CLASS_DDR_DATA_SIZE)
#define TMU_DDR_DATA_SIZE	(32 * SZ_1K)
#define TMU_LLM_BASEADDR	(TMU_DDR_DATA_BASEADDR + TMU_DDR_DATA_SIZE)
#define TMU_LLM_QUEUE_LEN	(8 * 512)	/* power of two, >= 16*8 bytes */
#define TMU_LLM_SIZE		(4 * 16 * TMU_LLM_QUEUE_LEN) /* 4 TMUs * 16 queues */
#define DDR_MAX_SIZE		(TMU_LLM_BASEADDR + TMU_LLM_SIZE)

/* LMEM sub-layout (same source) */
#define BMU1_LMEM_BASEADDR	0
#define BMU1_BUF_COUNT		256

/*
 * Field naming follows the 3.2.26 vendor driver's struct pfe where a
 * field already has an obvious counterpart, to keep porting mechanical.
 * HIF/firmware/net_device state (struct pfe_hif, pfe_eth, etc. in the
 * vendor tree) is added in the stages that need it (P4-P9).
 */
struct pfe {
	struct device *dev;

	void __iomem *apb_baseaddr;
	void __iomem *cbus_baseaddr;

	void __iomem *ddr_baseaddr;
	unsigned long ddr_phys_baseaddr;
	unsigned int ddr_size;

	void __iomem *iram_baseaddr;

	struct clk *clk_pfe;
	struct clk *clk_pfe_sys;
	struct clk *clk_gem_tx; /* shared GEM TX reference clock (Stage P7) */

	struct reset_control *rst_axi;
	struct reset_control *rst_core;

	int hif_irq;

	/*
	 * ELF section addresses discovered while loading firmware (Stage
	 * P4) -- diagnostic bookkeeping only (logged in pfe_firmware.c),
	 * not used for any address computation. The vendor driver's own
	 * struct pfe_ctrl keeps the same fields because it needs them to
	 * compute PE addresses via linker-shadow-section relative offsets;
	 * Stage P6 deliberately doesn't replicate that mechanism (see the
	 * banner comment in pfe_ctrl.h), so struct pfe_ctrl below has no
	 * use for these and they stay here instead of moving there.
	 */
	unsigned long class_dmem_sh;
	unsigned long class_pe_lmem_sh;
	unsigned long tmu_dmem_sh;
	unsigned long util_dmem_sh;
	unsigned long util_ddr_sh;

	/*
	 * CLASS PE DMEM address of the firmware's "phy_port[]" array
	 * (Stage P8 fix, pfe_eth.c) -- unlike the fields above, this one
	 * is load-bearing, not diagnostic: pfe_eth_open() writes each
	 * GEM's MAC address and interface index into it every time an
	 * interface comes up, confirmed on real hardware to be required
	 * for the CLASS PE firmware to forward any received frame to the
	 * host at all. Found the same way as pfe_ctrl's mailbox addresses
	 * (Stage P6) -- looked up by symbol name in the firmware ELF while
	 * it's still available, since pfe_firmware_init() releases it.
	 */
	unsigned long class_phy_port_dmem;

	struct pfe_hif hif;

	/* Control-message channel to the PE firmware (Stage P6) --
	 * PE start/stop and the DMEM<->DDR "PE request" mailbox protocol.
	 */
	struct pfe_ctrl ctrl;

	/* Registered clients, indexed by HIF_CLIENTS_MAX id (PFE_CL_GEM0,
	 * ...). Populated by hif_lib_client_register() (Stage P7-P9);
	 * hif_lib_indicate_client() (Stage P5) already needs to look this
	 * up from the Rx path, so the array itself is needed now even
	 * though nothing populates it yet.
	 */
	struct hif_client_s *hif_client[HIF_CLIENTS_MAX];
};

/*
 * This SoC only ever has one PFE instance, and the vendor driver's own
 * HIF/HIF-lib/GEMAC code is written throughout assuming direct access
 * to a single global instance rather than threading a pointer through
 * every call (e.g. hif_lib_indicate_client(), __hif_lib_xmit_pkt()).
 * Fighting that pattern would mean touching a very large fraction of
 * this port for no real benefit on hardware that can never have a
 * second instance, so it's kept -- but named g_pfe, not the vendor's
 * bare "pfe": every function in this port that already takes a
 * struct pfe * has been using the parameter name "pfe" since Stage P1
 * (confirmed working on real hardware many times over), and reusing
 * that exact name for the global would silently shadow it in every one
 * of those functions instead of raising a redeclaration error -- a
 * correctness trap for zero benefit. Only code with no local "pfe" of
 * its own (i.e. genuinely needs the single global instance) uses
 * g_pfe. Set once in pfe_platform_probe().
 */
extern struct pfe *g_pfe;

#endif /* _PFE_MOD_H_ */
