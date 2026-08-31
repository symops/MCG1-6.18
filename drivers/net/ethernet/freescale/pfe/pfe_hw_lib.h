/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Low-level CBUS register access layer shared between the PFE's own
 * firmware (running on the CLASS/TMU/UTIL packet-processor cores) and
 * this host driver -- ported from the 3.2.26 vendor tree's
 * pfe/pfe/c2000/pfe.c and pfe/pfe/c2000/pfe/pfe.h (kmodules/mspd-c2k/pfe/
 * in symops/MCG1-3.2.26). Only the subset needed so far (block init/
 * enable/disable/reset for BMU/GPI/CLASS/TMU/UTIL) is ported; the
 * PE-memory-access, ELF-loading and GEMAC pieces of the original files
 * are deferred to the stages that need them (Stage P4, P7-P9).
 */
#ifndef _PFE_HW_LIB_H_
#define _PFE_HW_LIB_H_

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

#endif /* _PFE_HW_LIB_H_ */
