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

#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/string.h>

#include <asm/byteorder.h>

#include "pfe_mod.h"
#include "pfe_hw_lib.h"

void __iomem *cbus_base_addr;
void *ddr_base_addr;
unsigned long ddr_phys_base_addr;
unsigned int ddr_size;
unsigned int pfe_chip_rev;

static struct pe_info pe[MAX_PE];

#define PFE_POLL_ITERATIONS	2000000

void pfe_lib_init(void __iomem *cbus_base, void *ddr_base,
		   unsigned long ddr_phys_base, unsigned int size)
{
	int i;

	cbus_base_addr = cbus_base;
	ddr_base_addr = ddr_base;
	ddr_phys_base_addr = ddr_phys_base;
	ddr_size = size;

	for (i = CLASS0_ID; i <= CLASS5_ID; i++) {
		pe[i].dmem_base_addr = CLASS_DMEM_BASE_ADDR(i - CLASS0_ID);
		pe[i].pmem_base_addr = CLASS_IMEM_BASE_ADDR(i - CLASS0_ID);
		pe[i].pmem_size = CLASS_IMEM_SIZE;
		pe[i].mem_access_wdata = CLASS_MEM_ACCESS_WDATA;
		pe[i].mem_access_addr = CLASS_MEM_ACCESS_ADDR;
		pe[i].mem_access_rdata = CLASS_MEM_ACCESS_RDATA;
	}

	for (i = TMU0_ID; i <= TMU3_ID; i++) {
		pe[i].dmem_base_addr = TMU_DMEM_BASE_ADDR(i - TMU0_ID);
		pe[i].pmem_base_addr = TMU_IMEM_BASE_ADDR(i - TMU0_ID);
		pe[i].pmem_size = TMU_IMEM_SIZE;
		pe[i].mem_access_wdata = TMU_MEM_ACCESS_WDATA;
		pe[i].mem_access_addr = TMU_MEM_ACCESS_ADDR;
		pe[i].mem_access_rdata = TMU_MEM_ACCESS_RDATA;
	}

	pe[UTIL_ID].dmem_base_addr = UTIL_DMEM_BASE_ADDR;
	pe[UTIL_ID].mem_access_wdata = UTIL_MEM_ACCESS_WDATA;
	pe[UTIL_ID].mem_access_addr = UTIL_MEM_ACCESS_ADDR;
	pe[UTIL_ID].mem_access_rdata = UTIL_MEM_ACCESS_RDATA;
}

/**************************** PE memory access ***************************/

static void pe_mem_memcpy_to32(int id, u32 mem_access_addr, const void *src, unsigned int len)
{
	u32 offset = 0, val, addr;
	unsigned int len32 = len >> 2;
	unsigned int i;

	addr = mem_access_addr | PE_MEM_ACCESS_WRITE | PE_MEM_ACCESS_BYTE_ENABLE(0, 4);

	for (i = 0; i < len32; i++, offset += 4, src += 4) {
		val = *(u32 *)src;
		writel(cpu_to_be32(val), pe[id].mem_access_wdata);
		writel(addr + offset, pe[id].mem_access_addr);
	}

	len = len & 0x3;
	if (len) {
		val = 0;
		addr = (mem_access_addr | PE_MEM_ACCESS_WRITE | PE_MEM_ACCESS_BYTE_ENABLE(0, len)) + offset;

		for (i = 0; i < len; i++, src++)
			val |= (*(u8 *)src) << (8 * i);

		writel(cpu_to_be32(val), pe[id].mem_access_wdata);
		writel(addr, pe[id].mem_access_addr);
	}
}

void pe_dmem_memcpy_to32(int id, u32 dst, const void *src, unsigned int len)
{
	pe_mem_memcpy_to32(id, pe[id].dmem_base_addr | dst | PE_MEM_ACCESS_DMEM, src, len);
}

void pe_pmem_memcpy_to32(int id, u32 dst, const void *src, unsigned int len)
{
	pe_mem_memcpy_to32(id, pe[id].pmem_base_addr | (dst & (pe[id].pmem_size - 1)) |
			    PE_MEM_ACCESS_IMEM, src, len);
}

u32 pe_pmem_read(int id, u32 addr, u8 size)
{
	u32 offset = addr & 0x3;
	u32 mask = 0xffffffff >> ((4 - size) << 3);
	u32 val;

	addr = pe[id].pmem_base_addr | ((addr & ~0x3) & (pe[id].pmem_size - 1)) |
	       PE_MEM_ACCESS_IMEM | PE_MEM_ACCESS_BYTE_ENABLE(offset, size);

	writel(addr, pe[id].mem_access_addr);
	val = be32_to_cpu(readl(pe[id].mem_access_rdata));

	return (val >> (offset << 3)) & mask;
}

void pe_dmem_write(int id, u32 val, u32 addr, u8 size)
{
	u32 offset = addr & 0x3;

	addr = pe[id].dmem_base_addr | (addr & ~0x3) | PE_MEM_ACCESS_WRITE | PE_MEM_ACCESS_DMEM |
	       PE_MEM_ACCESS_BYTE_ENABLE(offset, size);

	/* Indirect access interface is byte swapping data being written */
	writel(cpu_to_be32(val << (offset << 3)), pe[id].mem_access_wdata);
	writel(addr, pe[id].mem_access_addr);
}

u32 pe_dmem_read(int id, u32 addr, u8 size)
{
	u32 offset = addr & 0x3;
	u32 mask = 0xffffffff >> ((4 - size) << 3);
	u32 val;

	addr = pe[id].dmem_base_addr | (addr & ~0x3) | PE_MEM_ACCESS_DMEM |
	       PE_MEM_ACCESS_BYTE_ENABLE(offset, size);

	writel(addr, pe[id].mem_access_addr);

	/* Indirect access interface is byte swapping data being read */
	val = be32_to_cpu(readl(pe[id].mem_access_rdata));

	return (val >> (offset << 3)) & mask;
}

void class_bus_write(u32 val, u32 addr, u8 size)
{
	u32 offset = addr & 0x3;

	writel((addr & CLASS_BUS_ACCESS_BASE_MASK), CLASS_BUS_ACCESS_BASE);

	addr = (addr & ~CLASS_BUS_ACCESS_BASE_MASK) | PE_MEM_ACCESS_WRITE | (size << 24);

	writel(cpu_to_be32(val << (offset << 3)), CLASS_BUS_ACCESS_WDATA);
	writel(addr, CLASS_BUS_ACCESS_ADDR);
}

u32 class_bus_read(u32 addr, u8 size)
{
	u32 offset = addr & 0x3;
	u32 mask = 0xffffffff >> ((4 - size) << 3);
	u32 val;

	writel((addr & CLASS_BUS_ACCESS_BASE_MASK), CLASS_BUS_ACCESS_BASE);

	addr = (addr & ~CLASS_BUS_ACCESS_BASE_MASK) | (size << 24);

	writel(addr, CLASS_BUS_ACCESS_ADDR);
	val = be32_to_cpu(readl(CLASS_BUS_ACCESS_RDATA));

	return (val >> (offset << 3)) & mask;
}

void class_pe_lmem_memcpy_to32(u32 dst, const void *src, unsigned int len)
{
	u32 len32 = len >> 2;
	unsigned int i;

	for (i = 0; i < len32; i++, src += 4, dst += 4)
		class_bus_write(*(u32 *)src, dst, 4);

	if (len & 0x2) {
		class_bus_write(*(u16 *)src, dst, 2);
		src += 2;
		dst += 2;
	}

	if (len & 0x1) {
		class_bus_write(*(u8 *)src, dst, 1);
		src++;
		dst++;
	}
}

void class_pe_lmem_memset(u32 dst, int val, unsigned int len)
{
	u32 len32 = len >> 2;
	unsigned int i;

	val = val | (val << 8) | (val << 16) | (val << 24);

	for (i = 0; i < len32; i++, dst += 4)
		class_bus_write(val, dst, 4);

	if (len & 0x2) {
		class_bus_write(val, dst, 2);
		dst += 2;
	}

	if (len & 0x1) {
		class_bus_write(val, dst, 1);
		dst++;
	}
}

/*
 * UTIL program memory lives in DDR and is loaded as 64bit-swapped values
 * at 64bit-aligned locations -- unlike the other PEs' IMEM, which goes
 * through the indirect mem_access registers above.
 */
static void util_pmem_write(u32 val, void *addr, u8 size)
{
	void *addr64 = (void *)((unsigned long)addr & ~0x7);
	unsigned long off = 8 - ((unsigned long)addr & 0x7) - size;

	if (size == 4)
		writel(be32_to_cpu(val), addr64 + off);
	else
		writew(be16_to_cpu((u16)val), addr64 + off);
}

static void util_pmem_memcpy(void *dst, const void *src, unsigned int len)
{
	unsigned int len32;
	unsigned int i;

	if ((unsigned long)src & 0x2) {
		util_pmem_write(*(u16 *)src, dst, 2);
		src += 2;
		dst += 2;
		len -= 2;
	}

	len32 = len >> 2;

	for (i = 0; i < len32; i++, dst += 4, src += 4)
		util_pmem_write(*(u32 *)src, dst, 4);

	if (len & 0x2)
		util_pmem_write(*(u16 *)src, dst, len & 0x2);
}

/**************************** ELF section loading ***************************/

static int pe_load_pmem_section(int id, const void *data, const Elf32_Shdr *shdr)
{
	u32 offset = be32_to_cpu(shdr->sh_offset);
	u32 addr = be32_to_cpu(shdr->sh_addr);
	u32 size = be32_to_cpu(shdr->sh_size);
	u32 type = be32_to_cpu(shdr->sh_type);

	if (id == UTIL_ID) {
		pr_err("%s: unsupported pmem section for UTIL\n", __func__);
		return -EINVAL;
	}

	if (((unsigned long)(data + offset) & 0x3) != (addr & 0x3)) {
		pr_err("%s: load address(%x) and elf file address(%lx) don't have the same alignment\n",
		       __func__, addr, (unsigned long)data + offset);
		return -EINVAL;
	}

	if (addr & 0x1) {
		pr_err("%s: load address(%x) is not 16bit aligned\n", __func__, addr);
		return -EINVAL;
	}

	if (size & 0x1) {
		pr_err("%s: load size(%x) is not 16bit aligned\n", __func__, size);
		return -EINVAL;
	}

	switch (type) {
	case SHT_PROGBITS:
		pe_pmem_memcpy_to32(id, addr, data + offset, size);
		break;
	default:
		pr_err("%s: unsupported section type(%x)\n", __func__, type);
		return -EINVAL;
	}

	return 0;
}

static int pe_load_dmem_section(int id, const void *data, const Elf32_Shdr *shdr)
{
	u32 offset = be32_to_cpu(shdr->sh_offset);
	u32 addr = be32_to_cpu(shdr->sh_addr);
	u32 size = be32_to_cpu(shdr->sh_size);
	u32 type = be32_to_cpu(shdr->sh_type);
	u32 size32 = size >> 2;
	u32 i;

	if (((unsigned long)(data + offset) & 0x3) != (addr & 0x3)) {
		pr_err("%s: load address(%x) and elf file address(%lx) don't have the same alignment\n",
		       __func__, addr, (unsigned long)data + offset);
		return -EINVAL;
	}

	if (addr & 0x3) {
		pr_err("%s: load address(%x) is not 32bit aligned\n", __func__, addr);
		return -EINVAL;
	}

	switch (type) {
	case SHT_PROGBITS:
		pe_dmem_memcpy_to32(id, addr, data + offset, size);
		break;
	case SHT_NOBITS:
		for (i = 0; i < size32; i++, addr += 4)
			pe_dmem_write(id, 0, addr, 4);
		if (size & 0x3)
			pe_dmem_write(id, 0, addr, size & 0x3);
		break;
	default:
		pr_err("%s: unsupported section type(%x)\n", __func__, type);
		return -EINVAL;
	}

	return 0;
}

static int pe_load_ddr_section(int id, const void *data, const Elf32_Shdr *shdr)
{
	u32 offset = be32_to_cpu(shdr->sh_offset);
	u32 addr = be32_to_cpu(shdr->sh_addr);
	u32 size = be32_to_cpu(shdr->sh_size);
	u32 type = be32_to_cpu(shdr->sh_type);
	u32 flags = be32_to_cpu(shdr->sh_flags);

	switch (type) {
	case SHT_PROGBITS:
		if (flags & SHF_EXECINSTR) {
			if (id != UTIL_ID) {
				pr_err("%s: unsupported ddr section type(%x) for PE(%d)\n",
				       __func__, type, id);
				return -EINVAL;
			}

			if (((unsigned long)(data + offset) & 0x3) != (addr & 0x3)) {
				pr_err("%s: load address(%x) and elf file address(%lx) don't have the same alignment\n",
				       __func__, addr, (unsigned long)data + offset);
				return -EINVAL;
			}

			if (addr & 0x1) {
				pr_err("%s: load address(%x) is not 16bit aligned\n",
				       __func__, addr);
				return -EINVAL;
			}

			if (size & 0x1) {
				pr_err("%s: load length(%x) is not 16bit aligned\n",
				       __func__, size);
				return -EINVAL;
			}

			util_pmem_memcpy((void *)DDR_PHYS_TO_VIRT(addr), data + offset, size);
		} else {
			memcpy((void *)DDR_PHYS_TO_VIRT(addr), data + offset, size);
		}
		break;

	case SHT_NOBITS:
		memset((void *)DDR_PHYS_TO_VIRT(addr), 0, size);
		break;

	default:
		pr_err("%s: unsupported section type(%x)\n", __func__, type);
		return -EINVAL;
	}

	return 0;
}

static int pe_load_pe_lmem_section(int id, const void *data, const Elf32_Shdr *shdr)
{
	u32 offset = be32_to_cpu(shdr->sh_offset);
	u32 addr = be32_to_cpu(shdr->sh_addr);
	u32 size = be32_to_cpu(shdr->sh_size);
	u32 type = be32_to_cpu(shdr->sh_type);

	if (id > CLASS5_ID) {
		pr_err("%s: unsupported pe-lmem section type(%x) for PE(%d)\n",
		       __func__, type, id);
		return -EINVAL;
	}

	if (((unsigned long)(data + offset) & 0x3) != (addr & 0x3)) {
		pr_err("%s: load address(%x) and elf file address(%lx) don't have the same alignment\n",
		       __func__, addr, (unsigned long)data + offset);
		return -EINVAL;
	}

	if (addr & 0x3) {
		pr_err("%s: load address(%x) is not 32bit aligned\n", __func__, addr);
		return -EINVAL;
	}

	switch (type) {
	case SHT_PROGBITS:
		class_pe_lmem_memcpy_to32(addr, data + offset, size);
		break;
	case SHT_NOBITS:
		class_pe_lmem_memset(addr, 0, size);
		break;
	default:
		pr_err("%s: unsupported section type(%x)\n", __func__, type);
		return -EINVAL;
	}

	return 0;
}

int pe_load_elf_section(int id, const void *data, const Elf32_Shdr *shdr)
{
	u32 addr = be32_to_cpu(shdr->sh_addr);
	u32 size = be32_to_cpu(shdr->sh_size);

	if (IS_DMEM(addr, size))
		return pe_load_dmem_section(id, data, shdr);
	else if (IS_PMEM(addr, size))
		return pe_load_pmem_section(id, data, shdr);
	else if (IS_PFE_LMEM(addr, size))
		return 0; /* FIXME, matches vendor: not handled */
	else if (IS_PHYS_DDR(addr, size))
		return pe_load_ddr_section(id, data, shdr);
	else if (IS_PE_LMEM(addr, size))
		return pe_load_pe_lmem_section(id, data, shdr);

	pr_err("%s: unsupported memory range(%x)\n", __func__, addr);
	return 0;
}

/**************************** HIF (copy) block ***************************/

void hif_init(void)
{
	writel((HIF_RX_POLL_CTRL_CYCLE << 16) | HIF_TX_POLL_CTRL_CYCLE, HIF_POLL_CTRL);
}

void hif_tx_enable(void)
{
	writel(HIF_CTRL_DMA_EN, HIF_TX_CTRL);
	writel((readl(HIF_INT_ENABLE) | HIF_INT_EN | HIF_TXPKT_INT_EN), HIF_INT_ENABLE);
}

void hif_tx_disable(void)
{
	u32 hif_int;

	writel(0, HIF_TX_CTRL);

	hif_int = readl(HIF_INT_ENABLE);
	hif_int &= HIF_TXPKT_INT_EN;
	writel(hif_int, HIF_INT_ENABLE);
}

void hif_rx_enable(void)
{
	hif_rx_dma_start();
	writel((readl(HIF_INT_ENABLE) | HIF_INT_EN | HIF_RXPKT_INT_EN), HIF_INT_ENABLE);
}

void hif_rx_disable(void)
{
	u32 hif_int;

	writel(0, HIF_RX_CTRL);

	hif_int = readl(HIF_INT_ENABLE);
	hif_int &= HIF_RXPKT_INT_EN;
	writel(hif_int, HIF_INT_ENABLE);
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

/**************************** GEMAC ***************************/

void gemac_set_mode(void __iomem *base, int mode)
{
	u32 ctrl = readl(base + EMAC_CONTROL) & ~EMAC_MODE_MASK;
	u32 cfg = readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_SGMII_MODE_ENABLE;

	switch (mode) {
	case GMII:
		writel(ctrl | EMAC_GMII_MODE_ENABLE, base + EMAC_CONTROL);
		writel(cfg, base + EMAC_NETWORK_CONFIG);
		break;
	case RGMII:
		writel(ctrl | EMAC_RGMII_MODE_ENABLE, base + EMAC_CONTROL);
		writel(cfg, base + EMAC_NETWORK_CONFIG);
		break;
	case RMII:
		writel(ctrl | EMAC_RMII_MODE_ENABLE, base + EMAC_CONTROL);
		writel(cfg, base + EMAC_NETWORK_CONFIG);
		break;
	case SGMII:
		writel(ctrl | EMAC_RMII_MODE_DISABLE | EMAC_RGMII_MODE_DISABLE,
		       base + EMAC_CONTROL);
		writel(cfg | EMAC_SGMII_MODE_ENABLE, base + EMAC_NETWORK_CONFIG);
		break;
	case MII:
	default:
		writel(ctrl | EMAC_MII_MODE_ENABLE, base + EMAC_CONTROL);
		writel(cfg, base + EMAC_NETWORK_CONFIG);
		break;
	}
}

void gemac_set_speed(void __iomem *base, MAC_SPEED gem_speed)
{
	u32 val = readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_SPEED_MASK & ~EMAC_PCS_ENABLE;

	switch (gem_speed) {
	case SPEED_100M:
		val |= EMAC_SPEED_100;
		break;
	case SPEED_1000M:
		val |= EMAC_SPEED_1000;
		break;
	case SPEED_1000M_PCS:
		val |= EMAC_SPEED_1000 | EMAC_PCS_ENABLE;
		break;
	case SPEED_10M:
	default:
		break;
	}

	writel(val, base + EMAC_NETWORK_CONFIG);
}

void gemac_set_duplex(void __iomem *base, int duplex)
{
	u32 val = readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_DUPLEX_MASK;

	val |= (duplex == DUPLEX_HALF) ? EMAC_HALF_DUP : EMAC_FULL_DUP;

	writel(val, base + EMAC_NETWORK_CONFIG);
}

void gemac_set_config(void __iomem *base, GEMAC_CFG *cfg)
{
	gemac_set_mode(base, cfg->mode);
	gemac_set_speed(base, cfg->speed);
	gemac_set_duplex(base, cfg->duplex);
}

void gemac_enable(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONTROL) | EMAC_TX_ENABLE | EMAC_RX_ENABLE,
	       base + EMAC_NETWORK_CONTROL);
}

void gemac_disable(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONTROL) & ~(EMAC_TX_ENABLE | EMAC_RX_ENABLE),
	       base + EMAC_NETWORK_CONTROL);
}

void gemac_set_laddrN(void __iomem *base, MAC_ADDR *address, unsigned int entry_index)
{
	if (entry_index < 1 || entry_index > EMAC_SPEC_ADDR_MAX)
		return;

	entry_index--;

	if (entry_index < 4) {
		writel(address->bottom, base + (entry_index * 8) + EMAC_SPEC1_ADD_BOT);
		writel(address->top, base + (entry_index * 8) + EMAC_SPEC1_ADD_TOP);
	} else {
		writel(address->bottom, base + ((entry_index - 4) * 8) + EMAC_SPEC5_ADD_BOT);
		writel(address->top, base + ((entry_index - 4) * 8) + EMAC_SPEC5_ADD_TOP);
	}
}

void gemac_allow_broadcast(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_NO_BROADCAST, base + EMAC_NETWORK_CONFIG);
}

void gemac_no_broadcast(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) | EMAC_NO_BROADCAST, base + EMAC_NETWORK_CONFIG);
}

void gemac_enable_unicast(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) | EMAC_ENABLE_UNICAST, base + EMAC_NETWORK_CONFIG);
}

void gemac_disable_unicast(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_ENABLE_UNICAST, base + EMAC_NETWORK_CONFIG);
}

void gemac_enable_multicast(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) | EMAC_ENABLE_MULTICAST, base + EMAC_NETWORK_CONFIG);
}

void gemac_disable_multicast(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_ENABLE_MULTICAST, base + EMAC_NETWORK_CONFIG);
}

void gemac_enable_copy_all(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) | EMAC_ENABLE_COPY_ALL, base + EMAC_NETWORK_CONFIG);
}

void gemac_disable_copy_all(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_ENABLE_COPY_ALL, base + EMAC_NETWORK_CONFIG);
}

void gemac_set_hash(void __iomem *base, MAC_ADDR *hash)
{
	writel(hash->bottom, base + EMAC_HASH_BOT);
	writel(hash->top, base + EMAC_HASH_TOP);
}

void gemac_disable_fcs_rx(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_ENABLE_FCS_RX, base + EMAC_NETWORK_CONFIG);
}

void gemac_enable_1536_rx(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) | EMAC_ENABLE_1536_RX, base + EMAC_NETWORK_CONFIG);
}

void gemac_set_bus_width(void __iomem *base, int width)
{
	u32 val = readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_DATA_BUS_WIDTH_MASK;

	switch (width) {
	case 32:
		val |= EMAC_DATA_BUS_WIDTH_32;
		break;
	case 128:
		val |= EMAC_DATA_BUS_WIDTH_128;
		break;
	case 64:
	default:
		val |= EMAC_DATA_BUS_WIDTH_64;
		break;
	}

	writel(val, base + EMAC_NETWORK_CONFIG);
}

/*
 * Rx checksum offload also needs a CLASS-side switch (CLASS_L4_CHKSUM_ADDR)
 * telling the firmware to drop frames with a bad IPv4 checksum instead of
 * passing them up -- not just a GEMAC-side bit. Not called from Stage P7
 * (no NETIF_F_RXCSUM feature bit is declared on the net_device yet), but
 * ported now alongside the rest of pfe_gemac_init()'s register writes
 * rather than split across stages for no reason.
 */
void gemac_enable_rx_checksum_offload(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) | EMAC_ENABLE_CHKSUM_RX, base + EMAC_NETWORK_CONFIG);
	writel(readl(CLASS_L4_CHKSUM_ADDR) | IPV4_CHKSUM_DROP, CLASS_L4_CHKSUM_ADDR);
}

void gemac_disable_rx_checksum_offload(void __iomem *base)
{
	writel(readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_ENABLE_CHKSUM_RX, base + EMAC_NETWORK_CONFIG);
	writel(readl(CLASS_L4_CHKSUM_ADDR) & ~IPV4_CHKSUM_DROP, CLASS_L4_CHKSUM_ADDR);
}

void gemac_set_mdc_div(void __iomem *base, int mdc_div)
{
	u32 val = readl(base + EMAC_NETWORK_CONFIG) & ~EMAC_MDC_DIV_MASK;
	u32 div;

	switch (mdc_div) {
	case 8:
		div = 0;
		break;
	case 16:
		div = 1;
		break;
	case 32:
		div = 2;
		break;
	case 48:
		div = 3;
		break;
	case 96:
		div = 5;
		break;
	case 128:
		div = 6;
		break;
	case 224:
		div = 7;
		break;
	case 64:
	default:
		div = 4;
		break;
	}

	val |= div << 18;

	writel(val, base + EMAC_NETWORK_CONFIG);
}
