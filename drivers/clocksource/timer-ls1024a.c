// SPDX-License-Identifier: GPL-2.0
/*
 * Clocksource driver for the free-running counter in the LS1024A/Comcerto
 * 2000 SoC's "timer" register block (the same block, and the same syscon
 * regmap, as drivers/watchdog/ls1024a_wdt.c -- this uses a different
 * sub-timer within it, "timer2" in the 3.2.26 vendor tree's naming).
 *
 * Without this driver, this SoC's only registered clocksource is the
 * generic "jiffies" fallback, whose resolution is exactly one tick
 * (1/CONFIG_HZ = 4ms at this board's HZ=250) -- every CLOCK_MONOTONIC
 * timestamp, and therefore every RTT measured by userspace ping, is
 * quantized to that same 4ms step. The ARM Cortex-A9 Global Timer that
 * would normally provide a proper clocksource on this core is left
 * "status = disabled" in ls1024a.dtsi (see the comment there), and the
 * vendor kernel never used it either: its own arch/arm/mach-comcerto/
 * time.c drove sched_clock() and its clocksource from this exact
 * peripheral timer instead. This driver ports that same design.
 */

#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define TIMER2_LOW_BOUND	0x10
#define TIMER2_HIGH_BOUND	0x14
#define TIMER2_CTRL		0x18
#define TIMER2_CURRENT_COUNT	0x1c

struct ls1024a_clksrc {
	struct regmap *regs;
	struct clocksource cs;
};

static struct ls1024a_clksrc *to_ls1024a_clksrc(struct clocksource *cs)
{
	return container_of(cs, struct ls1024a_clksrc, cs);
}

static u64 ls1024a_clksrc_read(struct clocksource *cs)
{
	struct ls1024a_clksrc *priv = to_ls1024a_clksrc(cs);
	unsigned int val;

	regmap_read(priv->regs, TIMER2_CURRENT_COUNT, &val);

	return val;
}

static int ls1024a_clksrc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ls1024a_clksrc *priv;
	struct clk *clk;
	unsigned long rate;
	int res;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regs = syscon_node_to_regmap(dev->of_node->parent);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "Failed to get timer syscon\n");
		return PTR_ERR(priv->regs);
	}

	clk = devm_clk_get_enabled(dev->parent, NULL);
	if (IS_ERR(clk)) {
		dev_err(dev, "Failed to get timer clock\n");
		return PTR_ERR(clk);
	}

	rate = clk_get_rate(clk);
	if (!rate) {
		dev_err(dev, "Timer clock has zero rate\n");
		return -EINVAL;
	}

	/*
	 * Free-running up-counter: low bound 0, high bound the full 32-bit
	 * span, ctrl 0 -- the vendor tree's own proven-working configuration
	 * for this exact counter (arch/arm/mach-comcerto/time.c,
	 * comcerto_hwtimer_init()), replicated verbatim rather than guessed
	 * from the register names, since the ctrl bit's meaning is otherwise
	 * undocumented. It free-runs and wraps on its own; nothing here ever
	 * needs to rearm it.
	 */
	regmap_write(priv->regs, TIMER2_CTRL, 0);
	regmap_write(priv->regs, TIMER2_LOW_BOUND, 0);
	regmap_write(priv->regs, TIMER2_HIGH_BOUND, 0xffffffff);

	priv->cs.name = "ls1024a-timer2";
	priv->cs.rating = 250;
	priv->cs.read = ls1024a_clksrc_read;
	priv->cs.mask = CLOCKSOURCE_MASK(32);
	priv->cs.flags = CLOCK_SOURCE_IS_CONTINUOUS;

	res = clocksource_register_hz(&priv->cs, rate);
	if (res) {
		dev_err(dev, "Failed to register clocksource\n");
		return res;
	}

	dev_info(dev, "LS1024A hardware clocksource running at %lu Hz\n", rate);

	return 0;
}

static const struct of_device_id ls1024a_clksrc_of_match[] = {
	{ .compatible = "fsl,ls1024a-clksrc", },
	{ },
};
MODULE_DEVICE_TABLE(of, ls1024a_clksrc_of_match);

static struct platform_driver ls1024a_clksrc_driver = {
	.probe	= ls1024a_clksrc_probe,
	.driver = {
		.name		= "ls1024a-clksrc",
		.of_match_table	= ls1024a_clksrc_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ls1024a_clksrc_driver);
