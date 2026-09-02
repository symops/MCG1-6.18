// SPDX-License-Identifier: GPL-2.0
/*
 * PFE hardware block initialization -- ported from the 3.2.26 vendor
 * tree's pfe_ctrl/pfe_hw.c (kmodules/mspd-c2k/pfe/ in
 * symops/MCG1-3.2.26). Brings up BMU1/BMU2, CLASS, TMU, UTIL, and the
 * three EGPI blocks plus HGPI; the actual GEMAC/HIF/firmware bring-up
 * is added in later stages (P4-P9).
 */

#include <linux/io.h>

#include "pfe_mod.h"
#include "pfe_hw.h"
#include "pfe_hw_lib.h"

int pfe_hw_init(struct pfe *pfe)
{
	/*
	 * toe_mode: matches this board's real production configuration.
	 * The stock WD image loads this driver via /etc/modules with
	 * "pfe lro_mode=1 tx_qos=1 alloc_on_init=1 disable_wifi_offload=1"
	 * (confirmed by reading that file on the device) -- lro_mode=1 is
	 * what sets class_cfg.toe_mode=1 in the vendor driver
	 * (pfe_hw_init(): "if (lro_mode) class_cfg.toe_mode = 1;"), which
	 * OR's CLASS_TOE into the CLASS_ROUTE_MULTI register
	 * (class_set_config(), byte-for-byte ported into this port's own
	 * pfe_hw_lib.c already) -- a real CBUS register bit this port has
	 * never actually set, never checked against the vendor baseline
	 * (every prior round of register-level comparison read plenty of
	 * *other* CLASS registers, but never CLASS_ROUTE_MULTI itself).
	 * tx_qos (TX-only credit tracking) and alloc_on_init (purely
	 * host-side client-queue allocation timing, never touches a PFE
	 * register) don't have a built-in-driver equivalent worth adding;
	 * disable_wifi_offload only gates pfe_vwd.c code paths, and that
	 * whole driver was never ported (see pfe_eth.h's banner).
	 */
	CLASS_CFG class_cfg = {
		.pe_sys_clk_ratio = PE_SYS_CLK_RATIO,
		.route_table_baseaddr = pfe->ddr_phys_baseaddr + ROUTE_TABLE_BASEADDR,
		.route_table_hash_bits = ROUTE_TABLE_HASH_BITS,
		.toe_mode = 1,
	};

	TMU_CFG tmu_cfg = {
		.pe_sys_clk_ratio = PE_SYS_CLK_RATIO,
		.llm_base_addr = pfe->ddr_phys_baseaddr + TMU_LLM_BASEADDR,
		.llm_queue_len = TMU_LLM_QUEUE_LEN,
	};

	UTIL_CFG util_cfg = {
		.pe_sys_clk_ratio = PE_SYS_CLK_RATIO,
	};

	BMU_CFG bmu1_cfg = {
		.baseaddr = CBUS_VIRT_TO_PFE(LMEM_BASE_ADDR + BMU1_LMEM_BASEADDR),
		.count = BMU1_BUF_COUNT,
		.size = LMEM_BUF_SIZE_LN2,
	};

	BMU_CFG bmu2_cfg = {
		.baseaddr = pfe->ddr_phys_baseaddr + BMU2_DDR_BASEADDR,
		.count = BMU2_BUF_COUNT,
		.size = DDR_BUF_SIZE_LN2,
	};

	GPI_CFG egpi1_cfg = {
		.lmem_rtry_cnt = EGPI1_LMEM_RTRY_CNT,
		.tmlf_txthres = EGPI1_TMLF_TXTHRES,
		.aseq_len = EGPI1_ASEQ_LEN,
	};

	GPI_CFG egpi2_cfg = {
		.lmem_rtry_cnt = EGPI2_LMEM_RTRY_CNT,
		.tmlf_txthres = EGPI2_TMLF_TXTHRES,
		.aseq_len = EGPI2_ASEQ_LEN,
	};

	GPI_CFG egpi3_cfg = {
		.lmem_rtry_cnt = EGPI3_LMEM_RTRY_CNT,
		.tmlf_txthres = EGPI3_TMLF_TXTHRES,
		.aseq_len = EGPI3_ASEQ_LEN,
	};

	GPI_CFG hgpi_cfg = {
		.lmem_rtry_cnt = HGPI_LMEM_RTRY_CNT,
		.tmlf_txthres = HGPI_TMLF_TXTHRES,
		.aseq_len = HGPI_ASEQ_LEN,
	};

	dev_info(pfe->dev, "CLASS version: %x\n", readl(CLASS_VERSION));
	dev_info(pfe->dev, "TMU version: %x\n", readl(TMU_VERSION));
	dev_info(pfe->dev, "BMU1 version: %x\n", readl(BMU1_BASE_ADDR + BMU_VERSION));
	dev_info(pfe->dev, "BMU2 version: %x\n", readl(BMU2_BASE_ADDR + BMU_VERSION));
	dev_info(pfe->dev, "EGPI1 version: %x\n", readl(EGPI1_BASE_ADDR + GPI_VERSION));
	dev_info(pfe->dev, "EGPI2 version: %x\n", readl(EGPI2_BASE_ADDR + GPI_VERSION));
	dev_info(pfe->dev, "EGPI3 version: %x\n", readl(EGPI3_BASE_ADDR + GPI_VERSION));
	dev_info(pfe->dev, "HGPI version: %x\n", readl(HGPI_BASE_ADDR + GPI_VERSION));
	dev_info(pfe->dev, "HIF version: %x\n", readl(HIF_VERSION));
	dev_info(pfe->dev, "HIF NOCPY version: %x\n", readl(HIF_NOCPY_VERSION));
	dev_info(pfe->dev, "UTIL version: %x\n", readl(UTIL_VERSION));

	bmu_init(BMU1_BASE_ADDR, &bmu1_cfg);
	bmu_init(BMU2_BASE_ADDR, &bmu2_cfg);

	class_init(&class_cfg);
	tmu_init(&tmu_cfg);
	util_init(&util_cfg);

	gpi_init(EGPI1_BASE_ADDR, &egpi1_cfg);
	gpi_init(EGPI2_BASE_ADDR, &egpi2_cfg);
	gpi_init(EGPI3_BASE_ADDR, &egpi3_cfg);
	gpi_init(HGPI_BASE_ADDR, &hgpi_cfg);

	bmu_enable(BMU1_BASE_ADDR);
	bmu_enable(BMU2_BASE_ADDR);

	dev_info(pfe->dev, "%s: done\n", __func__);

	return 0;
}

void pfe_hw_exit(struct pfe *pfe)
{
	bmu_disable(BMU1_BASE_ADDR);
	bmu_reset(BMU1_BASE_ADDR);

	bmu_disable(BMU2_BASE_ADDR);
	bmu_reset(BMU2_BASE_ADDR);
}
