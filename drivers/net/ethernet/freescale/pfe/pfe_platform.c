// SPDX-License-Identifier: GPL-2.0
/*
 * PFE (Packet Forwarding Engine) platform driver for the Freescale/
 * Mindspeed LS1024A (Comcerto 2000) SoC.
 *
 * Stage P5 of the port (see Documentation/arm/ls1024a-wdmycloud.rst):
 * on top of the Stage P1-P4 resource/clock/reset/hw-block/firmware
 * skeleton, bring up the HIF DMA descriptor rings and ISR (see
 * pfe_hif.c/pfe_hif_lib.c). The IRQ is now requested by pfe_hif_init()
 * itself, once the ring/NAPI state it touches actually exists --
 * Stage P1-P4 requested it early with an always-IRQ_NONE stub
 * specifically because nothing was initialized yet; that stub and its
 * early devm_request_irq() call are gone now that there's a real
 * handler with real state to hand it (matches the vendor driver's own
 * ordering: pfe_hif_init() does its own request_irq() internally).
 *
 * Probe ordering also now matches the vendor's pfe_probe() exactly:
 * hw_init -> hif_lib_init -> hif_init -> firmware_init. This matters
 * because pfe_firmware_init() ends by enabling the class/tmu/util PE
 * cores (class_enable()/tmu_enable()/util_enable()) -- once enabled,
 * firmware can start driving HIF traffic, so HIF must already be
 * initialized and ready to receive before that happens. Stage P4 had
 * firmware_init running right after hw_init, before HIF existed at
 * all; reordered now, before real traffic is possible, rather than
 * leaving a latent bug for whenever Stage P7-P9 exercises real
 * traffic to discover the hard way.
 *
 * Stage P6 adds pfe_ctrl_init()/pfe_ctrl_exit(), the control-message
 * mailbox channel to the PE firmware (see pfe_ctrl.h) -- run after
 * firmware_init/before firmware_exit, again matching the vendor's own
 * ordering, since the mailbox addresses it uses are only known once
 * pfe_firmware_init() has parsed the firmware ELFs' symbol tables.
 *
 * Stage P7 adds pfe_eth_init()/pfe_eth_exit() (GEM0's net_device/MDIO/
 * PHY, see pfe_eth.h) last, after everything the HIF client-registration
 * path it drives (hif_lib_client_register(), Stage P7's own addition to
 * pfe_hif_lib.c) depends on already exists.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "pfe_mod.h"
#include "pfe_ctrl.h"
#include "pfe_eth.h"
#include "pfe_firmware.h"
#include "pfe_hif.h"
#include "pfe_hif_lib.h"
#include "pfe_hw.h"
#include "pfe_hw_lib.h"

struct pfe *g_pfe;

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

	/*
	 * IRAM is deliberately not mapped here -- see the comment on the
	 * pfe DT node for why (the region is already owned by the
	 * existing generic mmio-sram node, a second ioremap conflicts).
	 * pfe->iram_baseaddr stays NULL until a later stage wires up proper
	 * access through that node's genalloc pool.
	 */

	{
		struct device_node *mem_np;
		struct reserved_mem *rmem;

		mem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
		if (!mem_np)
			return dev_err_probe(dev, -ENODEV,
					      "Missing memory-region (ddr carve-out)\n");

		rmem = of_reserved_mem_lookup(mem_np);
		of_node_put(mem_np);
		if (!rmem)
			return dev_err_probe(dev, -ENODEV,
					      "Failed to look up ddr reserved-memory region\n");

		pfe->ddr_phys_baseaddr = rmem->base;
		pfe->ddr_size = rmem->size;

		/*
		 * Plain cacheable system DRAM (packet buffers/route table),
		 * not a device MMIO window -- memremap(), not ioremap().
		 * No live cross-master DMA happens yet at this stage (that
		 * starts in Stage P5), so this stage doesn't need to answer
		 * the cache-coherency question between the ARM cores and
		 * the PFE's own bus master; Stage P5 does and must revisit
		 * this mapping if real traffic shows corruption.
		 */
		pfe->ddr_baseaddr = devm_memremap(dev, pfe->ddr_phys_baseaddr,
						   pfe->ddr_size, MEMREMAP_WB);
		if (IS_ERR(pfe->ddr_baseaddr))
			return dev_err_probe(dev, PTR_ERR(pfe->ddr_baseaddr),
					      "Failed to map ddr carve-out\n");
	}

	/* Every function from here on that doesn't take a struct pfe *
	 * parameter of its own reaches this same instance via g_pfe.
	 */
	g_pfe = pfe;

	pfe_lib_init(pfe->cbus_baseaddr, pfe->ddr_baseaddr, pfe->ddr_phys_baseaddr,
		     pfe->ddr_size);

	ret = pfe_hw_init(pfe);
	if (ret)
		return dev_err_probe(dev, ret, "pfe_hw_init failed\n");

	ret = pfe_hif_lib_init(pfe);
	if (ret) {
		dev_err_probe(dev, ret, "pfe_hif_lib_init failed\n");
		goto err_hif_lib;
	}

	ret = pfe_hif_init(pfe);
	if (ret) {
		dev_err_probe(dev, ret, "pfe_hif_init failed\n");
		goto err_hif;
	}

	ret = pfe_firmware_init(pfe);
	if (ret) {
		dev_err_probe(dev, ret, "pfe_firmware_init failed\n");
		goto err_fw;
	}

	ret = pfe_ctrl_init(pfe);
	if (ret) {
		dev_err_probe(dev, ret, "pfe_ctrl_init failed\n");
		goto err_ctrl;
	}

	ret = pfe_eth_init(pfe);
	if (ret) {
		dev_err_probe(dev, ret, "pfe_eth_init failed\n");
		goto err_eth;
	}

	dev_info(dev, "PFE platform probed (apb=%p cbus=%p ddr=%pa/%u hif_irq=%d)\n",
		 pfe->apb_baseaddr, pfe->cbus_baseaddr, &pfe->ddr_phys_baseaddr,
		 pfe->ddr_size, pfe->hif_irq);

	return 0;

err_eth:
	pfe_ctrl_exit(pfe);
err_ctrl:
	pfe_firmware_exit(pfe);
err_fw:
	pfe_hif_exit(pfe);
err_hif:
	pfe_hif_lib_exit(pfe);
err_hif_lib:
	pfe_hw_exit(pfe);
	return ret;
}

static void pfe_platform_remove(struct platform_device *pdev)
{
	struct pfe *pfe = platform_get_drvdata(pdev);

	pfe_eth_exit(pfe);
	pfe_ctrl_exit(pfe);
	pfe_firmware_exit(pfe);
	pfe_hif_exit(pfe);
	pfe_hif_lib_exit(pfe);
	pfe_hw_exit(pfe);

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
