// SPDX-License-Identifier: GPL-2.0
/*
 * PFE firmware loader -- ported from the 3.2.26 vendor tree's
 * pfe_ctrl/pfe_firmware.c (kmodules/mspd-c2k/pfe/ in
 * symops/MCG1-3.2.26). Loads the three ELF firmware images
 * (class/tmu/util) via the standard request_firmware() API -- unlike
 * most of this port, this file needed essentially no adaptation: the
 * firmware-loading kernel API is unchanged from 3.2 through 6.18, and
 * the blobs themselves are opaque microcode with no host-kernel-version
 * coupling.
 *
 * request_firmware() here still needs plain synchronous calls, not the
 * async request_firmware_nowait() dance a firmware-dependent built-in
 * driver would normally need: this board has no initramfs and
 * CONFIG_FW_LOADER_USER_HELPER is off, and pfe's platform_driver probes
 * (Stage P1-P3) well before the root filesystem is mounted (confirmed
 * on real hardware: PFE probe at ~1.1s, EXT4 root mount at ~2.5-3.4s),
 * so a plain filesystem-path request_firmware() at probe time would
 * reliably fail with -ENOENT every single boot. Instead the three ELF
 * blobs are built directly into the kernel image via CONFIG_EXTRA_FIRMWARE
 * (see firmware/ls1024a-pfe/ and ls1024a_defconfig) -- built-in firmware
 * lookups are resolved before any filesystem path is even tried, so
 * this works regardless of when in boot it runs.
 *
 * barebox already runs its own copy of this same firmware-load sequence
 * at boot, before Linux ever starts (confirmed on real hardware, see
 * Documentation/arm/ls1024a-wdmycloud.rst) -- this function reloads the
 * PEs from scratch regardless, the same way the vendor driver always
 * did, so whatever state barebox left behind is simply overwritten.
 */

#include <linux/elf.h>
#include <linux/firmware.h>

#include "pfe_mod.h"
#include "pfe_ctrl.h"
#include "pfe_firmware.h"
#include "pfe_hw_lib.h"

static const Elf32_Shdr *get_elf_section_header(const struct firmware *fw, const char *section)
{
	const Elf32_Ehdr *elf_hdr = (const Elf32_Ehdr *)fw->data;
	const Elf32_Shdr *shdr, *shdr_shstr;
	Elf32_Off e_shoff = be32_to_cpu(elf_hdr->e_shoff);
	Elf32_Half e_shentsize = be16_to_cpu(elf_hdr->e_shentsize);
	Elf32_Half e_shnum = be16_to_cpu(elf_hdr->e_shnum);
	Elf32_Half e_shstrndx = be16_to_cpu(elf_hdr->e_shstrndx);
	Elf32_Off shstr_offset;
	Elf32_Word sh_name;
	const char *name;
	int i;

	shdr_shstr = (const Elf32_Shdr *)(fw->data + e_shoff + e_shstrndx * e_shentsize);
	shstr_offset = be32_to_cpu(shdr_shstr->sh_offset);

	for (i = 0; i < e_shnum; i++) {
		shdr = (const Elf32_Shdr *)(fw->data + e_shoff + i * e_shentsize);
		sh_name = be32_to_cpu(shdr->sh_name);
		name = (const char *)(fw->data + shstr_offset + sh_name);

		if (!strcmp(name, section))
			return shdr;
	}

	pr_err("%s: didn't find section %s\n", __func__, section);
	return NULL;
}

static unsigned long get_elf_section(const struct firmware *fw, const char *section)
{
	const Elf32_Shdr *shdr = get_elf_section_header(fw, section);

	if (shdr)
		return be32_to_cpu(shdr->sh_addr);
	return -1;
}

/*
 * Look up a symbol's address (st_value) in the firmware's .symtab.
 *
 * The vendor host driver never did this: it instead built its own
 * "shadow" copies of shared structures (sync_mailbox, msg_mailbox, ...)
 * into specially-named linker sections inside its own pfe_ctrl.ko
 * (CLASS_DMEM_SH()-family macros in pfe_ctrl_hal.h) and computed each
 * one's real PE-side address from the *relative offset* between that
 * shadow section and this driver's own equivalent shadow copy -- a
 * trick that needs the host driver built as a loadable module with its
 * own linker script, and needs the shared struct's definition to have
 * identical layout on both sides by construction.
 *
 * This port doesn't need that: the firmware ELFs are unstripped, so the
 * exact symbol (e.g. "sync_mbox", 8 bytes, matching struct
 * pe_sync_mailbype) can just be looked up directly and its st_value
 * used as-is (confirmed on the actual class_c2000.elf/tmu_c2000.elf/
 * util_c2000.elf blobs in this tree -- st_value is already an absolute
 * PE-side DMEM address, not section-relative). Simpler, and doesn't
 * require this driver to be built as a loadable module.
 */
static long get_elf_symbol_addr(const struct firmware *fw, const char *name)
{
	const Elf32_Shdr *symtab_shdr, *strtab_shdr;
	const Elf32_Sym *sym;
	Elf32_Word strtab_ndx;
	const char *strtab;
	unsigned int i, nsyms;

	symtab_shdr = get_elf_section_header(fw, ".symtab");
	if (!symtab_shdr)
		return -ENOENT;

	strtab_ndx = be32_to_cpu(symtab_shdr->sh_link);
	strtab_shdr = (const Elf32_Shdr *)(fw->data + be32_to_cpu(
			((const Elf32_Ehdr *)fw->data)->e_shoff) +
			strtab_ndx * be16_to_cpu(((const Elf32_Ehdr *)fw->data)->e_shentsize));
	strtab = fw->data + be32_to_cpu(strtab_shdr->sh_offset);

	sym = (const Elf32_Sym *)(fw->data + be32_to_cpu(symtab_shdr->sh_offset));
	nsyms = be32_to_cpu(symtab_shdr->sh_size) / sizeof(*sym);

	for (i = 0; i < nsyms; i++, sym++) {
		const char *sym_name = strtab + be32_to_cpu(sym->st_name);

		if (!strcmp(sym_name, name))
			return be32_to_cpu(sym->st_value);
	}

	pr_err("%s: symbol %s not found\n", __func__, name);
	return -ENOENT;
}

static void pfe_check_version_info(const struct firmware *fw)
{
	static const char *version;
	const Elf32_Shdr *shdr = get_elf_section_header(fw, ".version");

	if (!shdr) {
		pr_warn("WARNING: PFE firmware binaries from incompatible version\n");
		return;
	}

	if (!version) {
		/* First firmware loaded: use its version string as reference. */
		version = (const char *)(fw->data + be32_to_cpu(shdr->sh_offset));
		pr_info("PFE binary version: %s\n", version);
	} else if (strcmp(version, (const char *)(fw->data + be32_to_cpu(shdr->sh_offset)))) {
		pr_warn("WARNING: PFE firmware binaries from incompatible version\n");
	}
}

/**
 * pfe_load_elf() - load an ELF firmware image into a set of PEs
 * @pe_mask: mask of PE ids to load firmware into
 * @fw: the firmware image
 */
static int pfe_load_elf(int pe_mask, const struct firmware *fw)
{
	const Elf32_Ehdr *elf_hdr = (const Elf32_Ehdr *)fw->data;
	Elf32_Half sections = be16_to_cpu(elf_hdr->e_shnum);
	const Elf32_Shdr *shdr = (const Elf32_Shdr *)(fw->data + be32_to_cpu(elf_hdr->e_shoff));
	int id, section;
	int rc;

	if (strncmp(&elf_hdr->e_ident[EI_MAG0], ELFMAG, SELFMAG)) {
		pr_err("%s: incorrect elf magic number\n", __func__);
		return -EINVAL;
	}

	if (elf_hdr->e_ident[EI_CLASS] != ELFCLASS32) {
		pr_err("%s: incorrect elf class(%x)\n", __func__, elf_hdr->e_ident[EI_CLASS]);
		return -EINVAL;
	}

	if (elf_hdr->e_ident[EI_DATA] != ELFDATA2MSB) {
		pr_err("%s: incorrect elf data(%x)\n", __func__, elf_hdr->e_ident[EI_DATA]);
		return -EINVAL;
	}

	if (be16_to_cpu(elf_hdr->e_type) != ET_EXEC) {
		pr_err("%s: incorrect elf file type(%x)\n", __func__, be16_to_cpu(elf_hdr->e_type));
		return -EINVAL;
	}

	for (section = 0; section < sections; section++, shdr++) {
		if (!(be32_to_cpu(shdr->sh_flags) & (SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR)))
			continue;

		for (id = 0; id < MAX_PE; id++) {
			if (!(pe_mask & (1 << id)))
				continue;

			rc = pe_load_elf_section(id, fw->data, shdr);
			if (rc < 0)
				return rc;
		}
	}

	pfe_check_version_info(fw);

	return 0;
}

/*
 * Look up a firmware image's "sync_mbox"/"msg_mbox" symbols (see
 * get_elf_symbol_addr() above) and record their PE-side address for
 * every PE id in [first_id, last_id] -- all PEs loaded from the same
 * firmware image share the same firmware layout, hence the same
 * mailbox addresses (matching the vendor driver's own pfe_ctrl_init(),
 * which assigns the same looked-up address to every id in a PE-type's
 * range).
 */
static int pfe_ctrl_set_mailbox_addrs(struct pfe *pfe, const struct firmware *fw,
				       int first_id, int last_id)
{
	long sync_addr, msg_addr;
	int id;

	sync_addr = get_elf_symbol_addr(fw, "sync_mbox");
	if (sync_addr < 0)
		return sync_addr;

	msg_addr = get_elf_symbol_addr(fw, "msg_mbox");
	if (msg_addr < 0)
		return msg_addr;

	for (id = first_id; id <= last_id; id++) {
		pfe->ctrl.sync_mailbox_baseaddr[id] = sync_addr;
		pfe->ctrl.msg_mailbox_baseaddr[id] = msg_addr;
	}

	return 0;
}

int pfe_firmware_init(struct pfe *pfe)
{
	const struct firmware *class_fw, *tmu_fw, *util_fw;
	int rc;

	rc = request_firmware(&class_fw, CLASS_FIRMWARE_FILENAME, pfe->dev);
	if (rc) {
		dev_err(pfe->dev, "request firmware %s failed\n", CLASS_FIRMWARE_FILENAME);
		return rc;
	}

	rc = request_firmware(&tmu_fw, TMU_FIRMWARE_FILENAME, pfe->dev);
	if (rc) {
		dev_err(pfe->dev, "request firmware %s failed\n", TMU_FIRMWARE_FILENAME);
		goto err_tmu;
	}

	/*
	 * The vendor tree ships a second UTIL firmware variant
	 * ("util_c2000_revA0.elf") selected when the chip revision reads
	 * 0. That file isn't present anywhere in the GPL source drop we
	 * have, nor in the vendor kernel's own CONFIG_EXTRA_FIRMWARE
	 * list on this exact board (confirmed by reading
	 * /proc/config.gz there) -- and CHIP_REVISION() there reads 1,
	 * not 0 (see pfe_hw_lib.h's CHIP_REVISION() comment), so the
	 * vendor driver never actually requests it either. This
	 * non-revA0 file is simply the correct one for this hardware.
	 */
	rc = request_firmware(&util_fw, UTIL_FIRMWARE_FILENAME, pfe->dev);
	if (rc) {
		dev_err(pfe->dev, "request firmware %s failed\n", UTIL_FIRMWARE_FILENAME);
		goto err_util;
	}

	rc = pfe_load_elf(CLASS_MASK, class_fw);
	if (rc < 0) {
		dev_err(pfe->dev, "class firmware load failed\n");
		goto err_load;
	}
	pfe->class_dmem_sh = get_elf_section(class_fw, ".dmem_sh");
	pfe->class_pe_lmem_sh = get_elf_section(class_fw, ".pe_lmem_sh");
	dev_info(pfe->dev, "class firmware loaded %#lx %#lx\n",
		 pfe->class_dmem_sh, pfe->class_pe_lmem_sh);

	rc = pfe_ctrl_set_mailbox_addrs(pfe, class_fw, CLASS0_ID, CLASS_MAX_ID);
	if (rc < 0) {
		dev_err(pfe->dev, "failed to locate class mailbox symbols\n");
		goto err_load;
	}

	/*
	 * DMEM address of the firmware's phy_port[] array -- see the field
	 * comment on struct pfe.class_phy_port_dmem for why this is needed
	 * (Stage P8 fix, pfe_eth.c writes each GEM's MAC address/interface
	 * index here on open, required for the firmware to forward any
	 * received frame to the host at all).
	 */
	rc = get_elf_symbol_addr(class_fw, "phy_port");
	if (rc < 0) {
		dev_err(pfe->dev, "failed to locate phy_port symbol\n");
		goto err_load;
	}
	pfe->class_phy_port_dmem = rc;

	rc = pfe_load_elf(TMU_MASK, tmu_fw);
	if (rc < 0) {
		dev_err(pfe->dev, "tmu firmware load failed\n");
		goto err_load;
	}
	pfe->tmu_dmem_sh = get_elf_section(tmu_fw, ".dmem_sh");
	dev_info(pfe->dev, "tmu firmware loaded %#lx\n", pfe->tmu_dmem_sh);

	rc = pfe_ctrl_set_mailbox_addrs(pfe, tmu_fw, TMU0_ID, TMU_MAX_ID);
	if (rc < 0) {
		dev_err(pfe->dev, "failed to locate tmu mailbox symbols\n");
		goto err_load;
	}

	rc = pfe_load_elf(UTIL_MASK, util_fw);
	if (rc < 0) {
		dev_err(pfe->dev, "util firmware load failed\n");
		goto err_load;
	}
	pfe->util_dmem_sh = get_elf_section(util_fw, ".dmem_sh");
	pfe->util_ddr_sh = get_elf_section(util_fw, ".ddr_sh");
	dev_info(pfe->dev, "util firmware loaded %#lx\n", pfe->util_dmem_sh);

	rc = pfe_ctrl_set_mailbox_addrs(pfe, util_fw, UTIL_ID, UTIL_ID);
	if (rc < 0) {
		dev_err(pfe->dev, "failed to locate util mailbox symbols\n");
		goto err_load;
	}

	util_enable();
	tmu_enable(0xf);
	class_enable();

err_load:
	release_firmware(util_fw);
err_util:
	release_firmware(tmu_fw);
err_tmu:
	release_firmware(class_fw);

	return rc;
}

void pfe_firmware_exit(struct pfe *pfe)
{
	class_disable();
	tmu_disable(0xf);
	util_disable();
}
