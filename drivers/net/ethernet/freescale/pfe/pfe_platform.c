// SPDX-License-Identifier: GPL-2.0
/*
 * PFE (Packet Forwarding Engine) platform driver for the Freescale/
 * Mindspeed LS1024A (Comcerto 2000) SoC.
 *
 * Stage P1 of the port (see Documentation/arm/ls1024a-wdmycloud.rst):
 * bring up the "apb"/"axi" MMIO windows, the "pfe"/"pfe_sys" clocks and
 * the "axi"/"core" resets, and request (but do not yet arm) the "hif"
 * IRQ. None of the actual PFE hardware blocks (CLASS/TMU/UTIL/HIF/EMAC)
 * are touched yet -- that starts in later stages.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "pfe_mod.h"

static irqreturn_t pfe_hif_isr(int irq, void *dev_id)
{
	/*
	 * Nothing in the PFE hardware is initialized yet at this stage,
	 * so this interrupt should never actually fire -- if it does,
	 * that is itself a diagnostic finding, not expected operation.
	 */
	return IRQ_NONE;
}

static int pfe_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pfe *pfe;
	int ret;

	pfe = devm_kzalloc(dev, sizeof(*pfe), GFP_KERNEL);
	if (!pfe)
		return -ENOMEM;

	pfe->dev = dev;
	platform_set_drvdata(pdev, pfe);

	pfe->apb_baseaddr = devm_platform_ioremap_resource_byname(pdev, "apb");
	if (IS_ERR(pfe->apb_baseaddr))
		return dev_err_probe(dev, PTR_ERR(pfe->apb_baseaddr),
				      "Failed to map apb resource\n");

	pfe->cbus_baseaddr = devm_platform_ioremap_resource_byname(pdev, "axi");
	if (IS_ERR(pfe->cbus_baseaddr))
		return dev_err_probe(dev, PTR_ERR(pfe->cbus_baseaddr),
				      "Failed to map axi (cbus) resource\n");

	pfe->clk_pfe = devm_clk_get_enabled(dev, "pfe");
	if (IS_ERR(pfe->clk_pfe))
		return dev_err_probe(dev, PTR_ERR(pfe->clk_pfe),
				      "Failed to get/enable pfe clock\n");

	pfe->clk_pfe_sys = devm_clk_get_enabled(dev, "pfe_sys");
	if (IS_ERR(pfe->clk_pfe_sys))
		return dev_err_probe(dev, PTR_ERR(pfe->clk_pfe_sys),
				      "Failed to get/enable pfe_sys clock\n");

	pfe->rst_axi = devm_reset_control_get_exclusive(dev, "axi");
	if (IS_ERR(pfe->rst_axi))
		return dev_err_probe(dev, PTR_ERR(pfe->rst_axi),
				      "Failed to get axi reset\n");

	pfe->rst_core = devm_reset_control_get_exclusive(dev, "core");
	if (IS_ERR(pfe->rst_core))
		return dev_err_probe(dev, PTR_ERR(pfe->rst_core),
				      "Failed to get core reset\n");

	ret = reset_control_deassert(pfe->rst_axi);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to deassert axi reset\n");

	ret = reset_control_deassert(pfe->rst_core);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to deassert core reset\n");

	pfe->hif_irq = platform_get_irq_byname(pdev, "hif");
	if (pfe->hif_irq < 0)
		return pfe->hif_irq;

	ret = devm_request_irq(dev, pfe->hif_irq, pfe_hif_isr, 0,
				"pfe_hif", pfe);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request hif IRQ\n");

	dev_info(dev, "PFE platform skeleton probed (apb=%p cbus=%p hif_irq=%d)\n",
		 pfe->apb_baseaddr, pfe->cbus_baseaddr, pfe->hif_irq);

	return 0;
}

static void pfe_platform_remove(struct platform_device *pdev)
{
	struct pfe *pfe = platform_get_drvdata(pdev);

	reset_control_assert(pfe->rst_core);
	reset_control_assert(pfe->rst_axi);
}

static const struct of_device_id pfe_platform_of_match[] = {
	{ .compatible = "fsl,ls1024a-pfe" },
	{ }
};
MODULE_DEVICE_TABLE(of, pfe_platform_of_match);

static struct platform_driver pfe_platform_driver = {
	.probe = pfe_platform_probe,
	.remove = pfe_platform_remove,
	.driver = {
		.name = "fsl-ls1024a-pfe",
		.of_match_table = pfe_platform_of_match,
	},
};
module_platform_driver(pfe_platform_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Freescale LS1024A PFE platform driver");
