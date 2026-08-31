/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PFE_HW_H_
#define _PFE_HW_H_

/* SYS/AXI = 250MHz, HFE = 500MHz -- matches the vendor tree's non-PCI
 * (CONFIG_PLATFORM_PCI unset) clock ratio; the PCI-variant's 40MHz/40MHz
 * ratio is for a different board and not applicable here.
 */
#define PE_SYS_CLK_RATIO	1

int pfe_hw_init(struct pfe *pfe);
void pfe_hw_exit(struct pfe *pfe);

#endif /* _PFE_HW_H_ */
