/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PFE_MOD_H_
#define _PFE_MOD_H_

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/reset.h>
#include <linux/sizes.h>

#include "pfe_cbus.h"

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

	struct reset_control *rst_axi;
	struct reset_control *rst_core;

	int hif_irq;

	/*
	 * ELF section addresses discovered while loading firmware (Stage
	 * P4), needed later to talk to each PE's "shared" memory region.
	 * Vendor driver keeps these in a separate struct pfe_ctrl ctrl;
	 * member -- move them there once Stage P6 introduces it, rather
	 * than inventing that struct's shape ahead of actually needing it.
	 */
	unsigned long class_dmem_sh;
	unsigned long class_pe_lmem_sh;
	unsigned long tmu_dmem_sh;
	unsigned long util_dmem_sh;
	unsigned long util_ddr_sh;
};

#endif /* _PFE_MOD_H_ */
