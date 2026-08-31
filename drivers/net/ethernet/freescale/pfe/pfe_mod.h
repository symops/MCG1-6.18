/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PFE_MOD_H_
#define _PFE_MOD_H_

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/reset.h>

/*
 * Stage P1: platform_driver skeleton only -- MMIO windows, clocks and
 * resets are brought up, but none of the actual PFE hardware blocks
 * (CLASS/TMU/UTIL/HIF/EMAC) are touched yet. Field naming follows the
 * 3.2.26 vendor driver's struct pfe where a field already has an
 * obvious counterpart, to keep later stages' porting mechanical.
 */
struct pfe {
	struct device *dev;

	void __iomem *apb_baseaddr;
	void __iomem *cbus_baseaddr;

	struct clk *clk_pfe;
	struct clk *clk_pfe_sys;

	struct reset_control *rst_axi;
	struct reset_control *rst_core;

	int hif_irq;
};

#endif /* _PFE_MOD_H_ */
