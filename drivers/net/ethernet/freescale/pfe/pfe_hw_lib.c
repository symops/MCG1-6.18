// SPDX-License-Identifier: GPL-2.0
/*
 * Low-level CBUS register access layer -- see pfe_hw_lib.h. Ported from
 * the 3.2.26 vendor tree's pfe/pfe/c2000/pfe.c (kmodules/mspd-c2k/pfe/
 * in symops/MCG1-3.2.26), keeping only the BMU/GPI/CLASS/TMU/UTIL
 * init/enable/disable/reset functions this stage needs.
 *
 * Every polling loop here is bounded by an iteration count rather than
 * left to spin forever: an unbounded "while (!(readl(...) & DONE)) ;"
 * copied verbatim from the vendor driver would hang the whole board
 * with no serial output if the hardware doesn't respond as expected,
 * burning a real-hardware round-trip for nothing (this project's
 * SPI-NOR port hit exactly this class of problem -- see
 * Documentation/arm/ls1024a-wdmycloud.rst).
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/string.h>

#include "pfe_mod.h"
#include "pfe_hw_lib.h"

void __iomem *cbus_base_addr;
void *ddr_base_addr;
unsigned long ddr_phys_base_addr;
unsigned int ddr_size;

#define PFE_POLL_ITERATIONS	2000000

void pfe_lib_init(void __iomem *cbus_base, void *ddr_base,
		   unsigned long ddr_phys_base, unsigned int size)
{
	cbus_base_addr = cbus_base;
	ddr_base_addr = ddr_base;
	ddr_phys_base_addr = ddr_phys_base;
	ddr_size = size;
}

/**************************** BMU ***************************/

void bmu_reset(void __iomem *base)
{
	unsigned int i = 0;

	writel(CORE_SW_RESET, base + BMU_CTRL);

	while (readl(base + BMU_CTRL) & CORE_SW_RESET) {
		if (++i > PFE_POLL_ITERATIONS) {
			pr_err("%s: timed out waiting for CORE_SW_RESET to clear (BMU_CTRL=0x%x)\n",
			       __func__, readl(base + BMU_CTRL));
			break;
		}
	}
}

void bmu_enable(void __iomem *base)
{
	writel(CORE_ENABLE, base + BMU_CTRL);
}

void bmu_disable(void __iomem *base)
{
	writel(CORE_DISABLE, base + BMU_CTRL);
}

void bmu_set_config(void __iomem *base, BMU_CFG *cfg)
{
	writel(cfg->baseaddr, base + BMU_UCAST_BASE_ADDR);
	writel(cfg->count & 0xffff, base + BMU_UCAST_CONFIG);
	writel(cfg->size & 0xffff, base + BMU_BUF_SIZE);

	/* Interrupts are never used */
	writel(0x0, base + BMU_INT_ENABLE);
}

void bmu_init(void __iomem *base, BMU_CFG *cfg)
{
	bmu_disable(base);
	bmu_set_config(base, cfg);
	bmu_reset(base);
}

/**************************** GPI ***************************/

void gpi_reset(void __iomem *base)
{
	writel(CORE_SW_RESET, base + GPI_CTRL);
}

void gpi_enable(void __iomem *base)
{
	writel(CORE_ENABLE, base + GPI_CTRL);
}

void gpi_disable(void __iomem *base)
{
	writel(CORE_DISABLE, base + GPI_CTRL);
}

void gpi_set_config(void __iomem *base, GPI_CFG *cfg)
{
	writel(CBUS_VIRT_TO_PFE(BMU1_BASE_ADDR + BMU_ALLOC_CTRL), base + GPI_LMEM_ALLOC_ADDR);
	writel(CBUS_VIRT_TO_PFE(BMU1_BASE_ADDR + BMU_FREE_CTRL), base + GPI_LMEM_FREE_ADDR);
	writel(CBUS_VIRT_TO_PFE(BMU2_BASE_ADDR + BMU_ALLOC_CTRL), base + GPI_DDR_ALLOC_ADDR);
	writel(CBUS_VIRT_TO_PFE(BMU2_BASE_ADDR + BMU_FREE_CTRL), base + GPI_DDR_FREE_ADDR);
	writel(CBUS_VIRT_TO_PFE(CLASS_INQ_PKTPTR), base + GPI_CLASS_ADDR);
	writel(DDR_HDR_SIZE, base + GPI_DDR_DATA_OFFSET);
	writel(LMEM_HDR_SIZE, base + GPI_LMEM_DATA_OFFSET);
	writel(0, base + GPI_LMEM_SEC_BUF_DATA_OFFSET);
	writel(0, base + GPI_DDR_SEC_BUF_DATA_OFFSET);
	writel((DDR_HDR_SIZE << 16) | LMEM_HDR_SIZE, base + GPI_HDR_SIZE);
	writel((DDR_BUF_SIZE << 16) | LMEM_BUF_SIZE, base + GPI_BUF_SIZE);

	writel(((cfg->lmem_rtry_cnt << 16) | (GPI_DDR_BUF_EN << 1) | GPI_LMEM_BUF_EN),
	       base + GPI_RX_CONFIG);
	writel(cfg->tmlf_txthres, base + GPI_TMLF_TX);
	writel(cfg->aseq_len, base + GPI_DTX_ASEQ);
	writel(1, base + GPI_TOE_CHKSUM_EN);
}

void gpi_init(void __iomem *base, GPI_CFG *cfg)
{
	gpi_reset(base);
	gpi_disable(base);
	gpi_set_config(base, cfg);
}

/**************************** CLASS ***************************/

void class_reset(void)
{
	writel(CORE_SW_RESET, CLASS_TX_CTRL);
}

void class_enable(void)
{
	writel(CORE_ENABLE, CLASS_TX_CTRL);
}

void class_disable(void)
{
	writel(CORE_DISABLE, CLASS_TX_CTRL);
}

void class_set_config(CLASS_CFG *cfg)
{
	u32 val;

	/* Initialize route table */
	memset((void *)DDR_PHYS_TO_VIRT(cfg->route_table_baseaddr), 0,
	       (1 << cfg->route_table_hash_bits) * CLASS_ROUTE_SIZE);

	writel(cfg->pe_sys_clk_ratio, CLASS_PE_SYS_CLK_RATIO);

	writel((DDR_HDR_SIZE << 16) | LMEM_HDR_SIZE, CLASS_HDR_SIZE);
	writel(LMEM_BUF_SIZE, CLASS_LMEM_BUF_SIZE);
	writel(CLASS_ROUTE_ENTRY_SIZE(CLASS_ROUTE_SIZE) |
	       CLASS_ROUTE_HASH_SIZE(cfg->route_table_hash_bits),
	       CLASS_ROUTE_HASH_ENTRY_SIZE);
	writel(HIF_PKT_CLASS_EN | HIF_PKT_OFFSET(sizeof(struct hif_hdr)), CLASS_HIF_PARSE);

	val = HASH_CRC_PORT_IP | QB2BUS_LE;
	if (cfg->toe_mode)
		val |= CLASS_TOE;

	writel(val, CLASS_ROUTE_MULTI);

	writel(cfg->route_table_baseaddr, CLASS_ROUTE_TABLE_BASE);
	writel(CLASS_PE0_RO_DM_ADDR0_VAL, CLASS_PE0_RO_DM_ADDR0);
	writel(CLASS_PE0_RO_DM_ADDR1_VAL, CLASS_PE0_RO_DM_ADDR1);
	writel(CLASS_PE0_QB_DM_ADDR0_VAL, CLASS_PE0_QB_DM_ADDR0);
	writel(CLASS_PE0_QB_DM_ADDR1_VAL, CLASS_PE0_QB_DM_ADDR1);
	writel(CBUS_VIRT_TO_PFE(TMU_PHY_INQ_PKTPTR), CLASS_TM_INQ_ADDR);

	writel(23, CLASS_AFULL_THRES);
	writel(23, CLASS_TSQ_FIFO_THRES);

	writel(24, CLASS_MAX_BUF_CNT);
	writel(24, CLASS_TSQ_MAX_CNT);
}

void class_init(CLASS_CFG *cfg)
{
	class_reset();
	class_disable();
	class_set_config(cfg);
}

/**************************** TMU ***************************/

void tmu_reset(void)
{
	writel(SW_RESET, TMU_CTRL);
}

void tmu_enable(u32 pe_mask)
{
	writel(readl(TMU_TX_CTRL) | (pe_mask & 0xF), TMU_TX_CTRL);
}

void tmu_disable(u32 pe_mask)
{
	writel(readl(TMU_TX_CTRL) & ~(pe_mask & 0xF), TMU_TX_CTRL);
}

void tmu_init(TMU_CFG *cfg)
{
	unsigned int i;
	int q, phyno;

	/* keep in soft reset */
	writel(SW_RESET, TMU_CTRL);
	writel(0x3, TMU_SYS_GENERIC_CONTROL);
	writel(750, TMU_INQ_WATERMARK);
	writel(CBUS_VIRT_TO_PFE(EGPI1_BASE_ADDR + GPI_INQ_PKTPTR), TMU_PHY0_INQ_ADDR);
	writel(CBUS_VIRT_TO_PFE(EGPI2_BASE_ADDR + GPI_INQ_PKTPTR), TMU_PHY1_INQ_ADDR);
	writel(CBUS_VIRT_TO_PFE(EGPI3_BASE_ADDR + GPI_INQ_PKTPTR), TMU_PHY2_INQ_ADDR);
	writel(CBUS_VIRT_TO_PFE(HGPI_BASE_ADDR + GPI_INQ_PKTPTR), TMU_PHY3_INQ_ADDR);
	writel(CBUS_VIRT_TO_PFE(HIF_NOCPY_RX_INQ0_PKTPTR), TMU_PHY4_INQ_ADDR);
	writel(CBUS_VIRT_TO_PFE(UTIL_INQ_PKTPTR), TMU_PHY5_INQ_ADDR);
	writel(CBUS_VIRT_TO_PFE(BMU2_BASE_ADDR + BMU_FREE_CTRL), TMU_BMU_INQ_ADDR);

	/* enabling all 10 schedulers [9:0] of each TDQ */
	writel(0x3FF, TMU_TDQ0_SCH_CTRL);
	writel(0x3FF, TMU_TDQ1_SCH_CTRL);
	writel(0x3FF, TMU_TDQ2_SCH_CTRL);
	writel(0x3FF, TMU_TDQ3_SCH_CTRL);

	writel(cfg->pe_sys_clk_ratio, TMU_PE_SYS_CLK_RATIO);

	/* Extra packet pointers will be stored from this address onwards */
	writel(cfg->llm_base_addr, TMU_LLM_BASE_ADDR);

	writel(cfg->llm_queue_len, TMU_LLM_QUE_LEN);
	writel(5, TMU_TDQ_IIFG_CFG);
	writel(DDR_BUF_SIZE, TMU_BMU_BUF_SIZE);

	writel(0x0, TMU_CTRL);

	pr_info("%s: mem init\n", __func__);
	writel(MEM_INIT, TMU_CTRL);
	for (i = 0; !(readl(TMU_CTRL) & MEM_INIT_DONE); i++) {
		if (i > PFE_POLL_ITERATIONS) {
			pr_err("%s: timed out waiting for MEM_INIT_DONE (TMU_CTRL=0x%x)\n",
			       __func__, readl(TMU_CTRL));
			break;
		}
	}

	pr_info("%s: lmem init\n", __func__);
	writel(LLM_INIT, TMU_CTRL);
	for (i = 0; !(readl(TMU_CTRL) & LLM_INIT_DONE); i++) {
		if (i > PFE_POLL_ITERATIONS) {
			pr_err("%s: timed out waiting for LLM_INIT_DONE (TMU_CTRL=0x%x)\n",
			       __func__, readl(TMU_CTRL));
			break;
		}
	}

	/* set up each queue for tail drop */
	for (phyno = 0; phyno < 4; phyno++) {
		for (q = 0; q < 16; q++) {
			u32 qdepth;

			writel((phyno << 8) | q, TMU_TEQ_CTRL);
			writel(1 << 22, TMU_TEQ_QCFG); /* Enable tail drop */
			qdepth = ((phyno == 3) || (q < 8)) ? 511 : 255;

			/*
			 * Workaround for the reordered packet and BMU2
			 * buffer leakage issue (vendor tree: "LOG: 68855").
			 */
			if (CHIP_REVISION() == 0)
				qdepth = 31;

			writel(qdepth << 18, TMU_TEQ_HW_PROB_CFG2);
			writel(qdepth >> 14, TMU_TEQ_HW_PROB_CFG3);
		}
	}
	/* Set TMU-3 queue 5 (LRO) in no-drop mode */
	writel((3 << 8) | 5, TMU_TEQ_CTRL);
	writel(0, TMU_TEQ_QCFG);

	writel(0x05, TMU_TEQ_DISABLE_DROPCHK);

	writel(0x0, TMU_CTRL);
}

/**************************** UTIL ***************************/

void util_reset(void)
{
	writel(CORE_SW_RESET, UTIL_TX_CTRL);
}

void util_enable(void)
{
	writel(CORE_ENABLE, UTIL_TX_CTRL);
}

void util_disable(void)
{
	writel(CORE_DISABLE, UTIL_TX_CTRL);
}

void util_init(UTIL_CFG *cfg)
{
	writel(cfg->pe_sys_clk_ratio, UTIL_PE_SYS_CLK_RATIO);
}
