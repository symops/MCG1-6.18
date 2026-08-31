/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Low-level CBUS register access layer shared between the PFE's own
 * firmware (running on the CLASS/TMU/UTIL packet-processor cores) and
 * this host driver -- ported from the 3.2.26 vendor tree's
 * pfe/pfe/c2000/pfe.c and pfe/pfe/c2000/pfe/pfe.h (kmodules/mspd-c2k/pfe/
 * in symops/MCG1-3.2.26). Only the subset needed so far (block init/
 * enable/disable/reset for BMU/GPI/CLASS/TMU/UTIL) is ported; the
 * PE-memory-access and ELF section loading are now ported too (Stage
 * P4, for pfe_firmware.c); the GEMAC pieces are still deferred to the
 * stages that need them (P7-P9).
 */
#ifndef _PFE_HW_LIB_H_
#define _PFE_HW_LIB_H_

#include <linux/elf.h>

#include <asm/system_info.h>

#include "pfe_cbus.h"

/*
 * Minimal forward declaration of the HIF packet header, ahead of the
 * full DMA ring/descriptor definitions in pfe_hif.h (Stage P5) --
 * class_set_config() only needs its size, to tell CLASS how many bytes
 * of each buffer to skip past on the way in.
 */
struct hif_hdr {
	u8 client_id;
	u8 qNo;
	u16 client_ctrl;
	u16 client_ctrl1;
};

static inline unsigned int CHIP_REVISION(void)
{
	return system_rev;
}

/*
 * Runtime CBUS/DDR base addresses, set once via pfe_lib_init().
 * CBUS_BASE_ADDR expanding to this variable (rather than a compile-time
 * constant) is what lets pfe_cbus.h's *_BASE_ADDR macros resolve to the
 * actual ioremap'd host virtual address.
 *
 * cbus_base_addr is a true MMIO register window (readl/writel only).
 * ddr_base_addr is plain, cacheable system DRAM carved out for packet
 * buffers/route tables -- mapped with memremap(), not ioremap(), and
 * accessed with ordinary pointer/memset/memcpy like any other kernel
 * memory (matching the vendor driver's own plain memset() on it).
 */
extern void __iomem *cbus_base_addr;
extern void *ddr_base_addr;
extern unsigned long ddr_phys_base_addr;
extern unsigned int ddr_size;

#define CBUS_BASE_ADDR		cbus_base_addr
#define DDR_PHYS_BASE_ADDR	ddr_phys_base_addr
#define DDR_BASE_ADDR		ddr_base_addr
#define DDR_SIZE		ddr_size

/* CBUS physical base address as seen by the PE's (fixed, SoC-defined --
 * not the same as this host's ioremap'd virtual address above).
 */
#define PFE_CBUS_PHYS_BASE_ADDR	0xc0000000

#define DDR_PHYS_TO_VIRT(p)	(((p) - DDR_PHYS_BASE_ADDR) + (unsigned long)DDR_BASE_ADDR)
#define DDR_VIRT_TO_PHYS(v)	(((unsigned long)(v) - (unsigned long)DDR_BASE_ADDR) + DDR_PHYS_BASE_ADDR)

#define CBUS_VIRT_TO_PFE(v)	(((unsigned long)(v) - (unsigned long)CBUS_BASE_ADDR) + PFE_CBUS_PHYS_BASE_ADDR)
#define CBUS_PFE_TO_VIRT(p)	(((p) - PFE_CBUS_PHYS_BASE_ADDR) + (unsigned long)CBUS_BASE_ADDR)

void pfe_lib_init(void __iomem *cbus_base, void *ddr_base,
		   unsigned long ddr_phys_base, unsigned int size);

void bmu_init(void __iomem *base, BMU_CFG *cfg);
void bmu_reset(void __iomem *base);
void bmu_enable(void __iomem *base);
void bmu_disable(void __iomem *base);
void bmu_set_config(void __iomem *base, BMU_CFG *cfg);

void gpi_init(void __iomem *base, GPI_CFG *cfg);
void gpi_reset(void __iomem *base);
void gpi_enable(void __iomem *base);
void gpi_disable(void __iomem *base);
void gpi_set_config(void __iomem *base, GPI_CFG *cfg);

void class_init(CLASS_CFG *cfg);
void class_reset(void);
void class_enable(void);
void class_disable(void);
void class_set_config(CLASS_CFG *cfg);

void tmu_reset(void);
void tmu_init(TMU_CFG *cfg);
void tmu_enable(u32 pe_mask);
void tmu_disable(u32 pe_mask);

void util_init(UTIL_CFG *cfg);
void util_reset(void);
void util_enable(void);
void util_disable(void);

/*
 * PE (packet-processor core) memory map and per-PE identification, ported
 * from pfe/pfe/c2000/pfe/pfe.h. This board's config matches the vendor
 * tree's non-PCI, non-dummy-TMU, UTIL-enabled branch throughout (6 CLASS
 * PEs, 4 TMU PEs, 1 UTIL PE -- UTIL confirmed active by the util firmware
 * actually loading on real hardware, see Documentation/arm/ls1024a-wdmycloud.rst).
 */
#define CLASS_DMEM_BASE_ADDR(i)	(0x00000000 | ((i) << 20))
#define CLASS_IMEM_BASE_ADDR(i)	(0x00000000 | ((i) << 20))
#define CLASS_DMEM_SIZE		0x00002000
#define CLASS_IMEM_SIZE		0x00008000

#define TMU_DMEM_BASE_ADDR(i)	(0x00000000 + ((i) << 20))
#define TMU_IMEM_BASE_ADDR(i)	(0x00000000 + ((i) << 20))
#define TMU_DMEM_SIZE		0x00000800
#define TMU_IMEM_SIZE		0x00002000

#define UTIL_DMEM_BASE_ADDR	0x00000000
#define UTIL_DMEM_SIZE		0x00002000

#define PE_LMEM_BASE_ADDR	0xc3010000
#define PE_LMEM_SIZE		0x8000
#define PE_LMEM_END		(PE_LMEM_BASE_ADDR + PE_LMEM_SIZE)

#define DMEM_BASE_ADDR		0x00000000
#define DMEM_SIZE		0x2000
#define DMEM_END		(DMEM_BASE_ADDR + DMEM_SIZE)

#define PMEM_BASE_ADDR		0x00010000
#define PMEM_SIZE		0x8000
#define PMEM_END		(PMEM_BASE_ADDR + PMEM_SIZE)

#define IS_DMEM(addr, len)	(((unsigned long)(addr) >= DMEM_BASE_ADDR) && (((unsigned long)(addr) + (len)) <= DMEM_END))
#define IS_PMEM(addr, len)	(((unsigned long)(addr) >= PMEM_BASE_ADDR) && (((unsigned long)(addr) + (len)) <= PMEM_END))
#define IS_PE_LMEM(addr, len)	(((unsigned long)(addr) >= PE_LMEM_BASE_ADDR) && (((unsigned long)(addr) + (len)) <= PE_LMEM_END))
#define IS_PFE_LMEM(addr, len)	(((unsigned long)(addr) >= CBUS_VIRT_TO_PFE(LMEM_BASE_ADDR)) && (((unsigned long)(addr) + (len)) <= CBUS_VIRT_TO_PFE(LMEM_END)))
#define IS_PHYS_DDR(addr, len)	(((unsigned long)(addr) >= DDR_PHYS_BASE_ADDR) && (((unsigned long)(addr) + (len)) <= DDR_PHYS_BASE_ADDR + DDR_SIZE))

enum {
	CLASS0_ID = 0,
	CLASS1_ID,
	CLASS2_ID,
	CLASS3_ID,
	CLASS4_ID,
	CLASS5_ID,
	TMU0_ID,
	TMU1_ID,
	TMU2_ID,
	TMU3_ID,
	UTIL_ID,
	MAX_PE
};

#define CLASS_MASK	((1 << CLASS0_ID) | (1 << CLASS1_ID) | (1 << CLASS2_ID) | \
			 (1 << CLASS3_ID) | (1 << CLASS4_ID) | (1 << CLASS5_ID))
#define TMU_MASK	((1 << TMU0_ID) | (1 << TMU1_ID) | (1 << TMU2_ID) | (1 << TMU3_ID))
#define UTIL_MASK	(1 << UTIL_ID)

/* PE information: virtual addresses of a PE's indirect memory-access
 * registers, needed by the generic pe_*_memcpy/pe_dmem_* helpers below.
 */
struct pe_info {
	u32 dmem_base_addr;
	u32 pmem_base_addr;
	u32 pmem_size;

	void __iomem *mem_access_wdata;
	void __iomem *mem_access_addr;
	void __iomem *mem_access_rdata;
};

void pe_dmem_memcpy_to32(int id, u32 dst, const void *src, unsigned int len);
void pe_pmem_memcpy_to32(int id, u32 dst, const void *src, unsigned int len);
u32 pe_pmem_read(int id, u32 addr, u8 size);
void pe_dmem_write(int id, u32 val, u32 addr, u8 size);
u32 pe_dmem_read(int id, u32 addr, u8 size);
void class_bus_write(u32 val, u32 addr, u8 size);
u32 class_bus_read(u32 addr, u8 size);
void class_pe_lmem_memcpy_to32(u32 dst, const void *src, unsigned int len);
void class_pe_lmem_memset(u32 dst, int val, unsigned int len);

int pe_load_elf_section(int id, const void *data, const Elf32_Shdr *shdr);

/**************************** HIF (copy) block ***************************/

void hif_init(void);
void hif_tx_enable(void);
void hif_tx_disable(void);
void hif_rx_enable(void);
void hif_rx_disable(void);

static inline void hif_rx_dma_start(void)
{
	writel(HIF_CTRL_DMA_EN | HIF_CTRL_BDP_CH_START_WSTB, HIF_RX_CTRL);
}

static inline void hif_tx_dma_start(void)
{
	writel(HIF_CTRL_DMA_EN | HIF_CTRL_BDP_CH_START_WSTB, HIF_TX_CTRL);
}

#endif /* _PFE_HW_LIB_H_ */
