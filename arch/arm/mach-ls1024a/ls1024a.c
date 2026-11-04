// SPDX-License-Identifier: GPL-2.0
/*
 * LS1024A DT board support code
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */

#include <linux/clocksource.h>
#include <linux/irqchip.h>

#include <asm/hardware/cache-l2x0.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

/*
 * PL310 (r3p2, 8 ways x 32 KB) aux control, matching the 3.2.26 vendor
 * comcerto_l2cc_init(): shared-attribute override (keeps normal
 * non-cacheable memory -- the PFE/HIF coherent buffers -- out of the L2)
 * and write-allocate override forced to "no write allocate" (bits 24:23 =
 * 01). Way size/associativity are left as the hardware reports them;
 * latencies and the DDR address filter come from the DT node.
 */
#define LS1024A_L2C_AUX_VAL	(L2C_AUX_CTRL_SHARED_OVERRIDE | BIT(23))
#define LS1024A_L2C_AUX_MASK	(~(BIT(24) | BIT(23)))

static void __init ls1024a_map_io(void)
{
	debug_ll_io_init();
}

static const char *const ls1024a_compat[] __initconst = {
	"fsl,ls1024a",
	NULL,
};

DT_MACHINE_START(LS1024A, "Freescale LS1024A")
	.map_io		= ls1024a_map_io,
	.dt_compat	= ls1024a_compat,
	.l2c_aux_val	= LS1024A_L2C_AUX_VAL,
	.l2c_aux_mask	= LS1024A_L2C_AUX_MASK,
MACHINE_END

