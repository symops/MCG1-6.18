/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PFE_FIRMWARE_H_
#define _PFE_FIRMWARE_H_

/*
 * Namespaced under "ls1024a-pfe/" both at build time (firmware/ls1024a-pfe/
 * in this tree, see CONFIG_EXTRA_FIRMWARE) and at runtime fallback
 * (/lib/firmware/ls1024a-pfe/) -- avoids any collision with a same-named
 * blob some other driver might request.
 */
#define CLASS_FIRMWARE_FILENAME	"ls1024a-pfe/class_c2000.elf"
#define TMU_FIRMWARE_FILENAME		"ls1024a-pfe/tmu_c2000.elf"
#define UTIL_FIRMWARE_FILENAME		"ls1024a-pfe/util_c2000.elf"

int pfe_firmware_init(struct pfe *pfe);
void pfe_firmware_exit(struct pfe *pfe);

#endif /* _PFE_FIRMWARE_H_ */
