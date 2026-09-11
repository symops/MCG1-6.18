// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cosmetic power-off for the WD My Cloud (Gen 1) / LS1024A.
 *
 * Modeled after symops/monarch-6.18's drivers/power/reset/wdmc-poweroff.c
 * (WD My Cloud Home / Duo, RTD1295/1296) -- same underlying situation:
 * neither mainline nor the vendor 3.2.26 GPL source implements a real
 * power-off for this board. Every file under arch/arm/mach-comcerto/ in
 * the vendor tree was checked for pm_power_off -- zero hits anywhere, and
 * arch/arm/mach-comcerto/reset.c only ever puts individual peripheral
 * blocks in/out of reset, never the whole board. `halt`/`poweroff` on
 * this hardware therefore just parks the CPU with the board still fully
 * powered: front-panel LEDs stay lit in whatever state they were last
 * set to.
 *
 * This driver does not attempt to cut real board power -- there is no
 * known way to do that from software on this hardware (no power-hold/
 * pwr_en GPIO was ever found, and unlike Monarch/Duo, no captured
 * stock-firmware boot log or vendor USB driver was available to hunt for
 * a VBUS-cut line on this board either -- so this driver doesn't attempt
 * that). It only quiets the one thing confirmed to be under this SoC's
 * own direct, already-understood control: the front-panel LEDs
 * (system_leds red/green/blue, wifi_leds yellow/blue -- see
 * ls1024a-wdmycloud.dts).
 *
 * Those 5 GPIO lines are already exclusively owned by the generic
 * gpio-leds driver, so requesting them again here as gpiod consumers
 * would collide with that existing claim. Instead, this driver reaches
 * the same "gpio" block's own output-data register directly through its
 * syscon regmap -- the same pattern drivers/watchdog/ls1024a_wdt.c and
 * drivers/clocksource/timer-ls1024a.c already use for their own
 * registers in sibling syscon blocks in this port, and the same
 * reasoning the Monarch/Duo driver gives for its own raw PWM OCD poke.
 */

#include <linux/bitops.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/processor.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

#define GPIO_OUTPUT_REG			0x0000
#define LS1024A_POWEROFF_MAX_LED_BITS	8

struct ls1024a_poweroff_data {
	struct regmap *gpio_regs;
	u32 led_bits[LS1024A_POWEROFF_MAX_LED_BITS];
	unsigned int n_led_bits;
};

static int ls1024a_poweroff_do_poweroff(struct sys_off_data *data)
{
	struct ls1024a_poweroff_data *priv = data->cb_data;
	u32 mask = 0;
	unsigned int i;

	for (i = 0; i < priv->n_led_bits; i++)
		mask |= BIT(priv->led_bits[i]);

	/* All LEDs on this board are active-high, so 0 == off. */
	regmap_update_bits(priv->gpio_regs, GPIO_OUTPUT_REG, mask, 0);

	/*
	 * This board has no real power-off hardware at all (see the file
	 * header), so this handler is the final stop, not a normal one that
	 * hands back control. Returning here would unwind through
	 * do_kernel_power_off() and machine_power_off() (arch/arm/kernel/
	 * reboot.c) back to __do_sys_reboot(), which calls do_exit(0) on
	 * PID 1 next -- fatal, since killing init panics the kernel.
	 * machine_halt() avoids exactly this with its own "while (1);"
	 * tail; machine_power_off() has no such fallback of its own
	 * because real power-off hardware is expected to take the CPU down
	 * before it would ever matter. Confirmed on real hardware: without
	 * this loop, `poweroff` genuinely panicked ("Attempted to kill
	 * init!") and rebooted a few seconds later instead of staying off.
	 * IRQs are already disabled at this point (machine_power_off()
	 * disables them before running this handler).
	 */
	for (;;)
		cpu_relax();

	return NOTIFY_DONE;
}

static int ls1024a_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ls1024a_poweroff_data *priv;
	int ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->gpio_regs = syscon_regmap_lookup_by_phandle(dev->of_node, "wdc,gpio-syscon");
	if (IS_ERR(priv->gpio_regs))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_regs),
				      "failed to get gpio syscon\n");

	ret = of_property_count_u32_elems(dev->of_node, "wdc,led-gpio-bits");
	if (ret <= 0 || ret > LS1024A_POWEROFF_MAX_LED_BITS)
		return dev_err_probe(dev, -EINVAL,
				      "bad or missing wdc,led-gpio-bits\n");
	priv->n_led_bits = ret;

	for (i = 0; i < ret; i++) {
		u32 bit;

		of_property_read_u32_index(dev->of_node, "wdc,led-gpio-bits", i, &bit);
		if (bit >= 32)
			return dev_err_probe(dev, -EINVAL,
					      "gpio bit %u out of range\n", bit);
		priv->led_bits[i] = bit;
	}

	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF,
					     SYS_OFF_PRIO_DEFAULT,
					     ls1024a_poweroff_do_poweroff, priv);
	if (ret)
		return dev_err_probe(dev, ret,
				      "failed to register poweroff handler\n");

	dev_info(dev, "registered cosmetic poweroff (%u led gpio bit(s))\n",
		 priv->n_led_bits);

	return 0;
}

static const struct of_device_id ls1024a_poweroff_of_match[] = {
	{ .compatible = "wdc,mcg1-poweroff" },
	{ }
};
MODULE_DEVICE_TABLE(of, ls1024a_poweroff_of_match);

static struct platform_driver ls1024a_poweroff_driver = {
	.probe = ls1024a_poweroff_probe,
	.driver = {
		.name = "ls1024a-poweroff",
		.of_match_table = ls1024a_poweroff_of_match,
	},
};
module_platform_driver(ls1024a_poweroff_driver);

MODULE_DESCRIPTION("Cosmetic power-off (front LEDs) for WD My Cloud (Gen 1)");
MODULE_LICENSE("GPL");
