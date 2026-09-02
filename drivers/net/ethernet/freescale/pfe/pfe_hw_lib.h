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
#include <linux/io.h>

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

/*
 * The vendor driver reads this from a real SoC-level chip-ID register
 * (COMCERTO_GPIO_DEVICE_ID_REG, mach-comcerto-2000.c's device_Init():
 * "system_rev = (readl(COMCERTO_GPIO_DEVICE_ID_REG) >> 24) & 0xf;") into
 * the generic ARM kernel's system_rev global -- ATAGS-era board-file
 * infrastructure this DT-only port never had a path to populate, so
 * system_rev silently stayed 0 here. That 0 is not a harmless "unknown"
 * default: control_qm.c's QM_update_qdepth() has a chip-rev-0-only
 * workaround ("LOG: 68855 -- workaround for the reordered packet and
 * BMU2 buffer leakage issue", forces every TMU queue's depth down to 31)
 * that tmu_init() below also carries -- with CHIP_REVISION() wrongly
 * always reading 0, this port was unconditionally applying that
 * rev-0-only workaround to hardware confirmed (by reading this exact
 * register from the vendor driver on this exact board -- see
 * Documentation/arm/ls1024a-wdmycloud.rst) to be rev 1, forcing every
 * queue down to a 31-entry depth instead of the correct 511/255 --
 * a real, previously undiagnosed root cause fully consistent with the
 * whole RX investigation's central symptom (BMU2 buffers accumulating,
 * never freed). pfe_chip_rev is set once, early in
 * pfe_platform_probe(), by reading that same physical register directly
 * (see the ioremap() there) -- independent of the generic system_rev
 * global, which nothing in this DT-boot kernel ever populates.
 */
extern unsigned int pfe_chip_rev;

static inline unsigned int CHIP_REVISION(void)
{
	return pfe_chip_rev;
}

/*
 * Runtime CBUS/DDR base addresses, set once via pfe_lib_init().
 * CBUS_BASE_ADDR expanding to this variable (rather than a compile-time
 * constant) is what lets pfe_cbus.h's *_BASE_ADDR macros resolve to the
 * actual ioremap'd host virtual address.
 *
 * cbus_base_addr is a true MMIO register window (readl/writel only).
 * ddr_base_addr is the DDR carve-out for packet buffers/firmware/route
 * table -- mapped uncached with ioremap() (matching the vendor driver's
 * own ioremap() of this same resource exactly; a plain memremap(...,
 * MEMREMAP_WB) here left CPU writes into it cache-stale from PFE's own
 * bus-master's point of view, confirmed as a real bug via real-hardware
 * testing -- see Documentation/arm/ls1024a-wdmycloud.rst), accessed with
 * ordinary pointer/memset/memcpy like any other memory.
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

/**************************** GEMAC ***************************/

/*
 * Subset of the vendor pfe.c's GEMAC block needed for Stage P7 (net_device/
 * MDIO/PHY bring-up for GEM0, no traffic path yet). gemac_reset() is a
 * no-op in the vendor driver itself (nothing to port); the full 4-entry
 * gemac_set_address()/gemac_get_address() and single-entry
 * gemac_set_laddr1..4()/gemac_clear_laddr*() accessors aren't needed since
 * pfe_eth.c only ever uses the parametrized gemac_set_laddrN().
 */
void gemac_set_mode(void __iomem *base, int mode);
void gemac_set_speed(void __iomem *base, MAC_SPEED gem_speed);
void gemac_set_duplex(void __iomem *base, int duplex);
void gemac_set_config(void __iomem *base, GEMAC_CFG *cfg);
void gemac_enable(void __iomem *base);
void gemac_disable(void __iomem *base);
void gemac_set_laddrN(void __iomem *base, MAC_ADDR *address, unsigned int entry_index);
void gemac_allow_broadcast(void __iomem *base);
void gemac_disable_unicast(void __iomem *base);
void gemac_disable_multicast(void __iomem *base);
void gemac_disable_fcs_rx(void __iomem *base);
void gemac_enable_1536_rx(void __iomem *base);
void gemac_set_bus_width(void __iomem *base, int width);
void gemac_enable_rx_checksum_offload(void __iomem *base);
void gemac_disable_rx_checksum_offload(void __iomem *base);
void gemac_set_mdc_div(void __iomem *base, int mdc_div);

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
#define CLASS_MAX_ID	CLASS5_ID
#define TMU_MASK	((1 << TMU0_ID) | (1 << TMU1_ID) | (1 << TMU2_ID) | (1 << TMU3_ID))
#define TMU_MAX_ID	TMU3_ID
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
