.. SPDX-License-Identifier: GPL-2.0

===========================================================
WD My Cloud (Gen 1) -- LS1024A/Comcerto 2000 port, Stage 1
===========================================================

This is a porting-progress note, not upstream-quality documentation.
It tracks the state of bringing v6.18.46 up on the WD My Cloud gen1
NAS (SoC: Mindspeed/Freescale "Comcerto 2000", later renamed
LS1024A after the Mindspeed/MACOM/NXP lineage), starting from a
working 3.2.26 vendor kernel (see symops/MCG1-3.2.26) and the WIP
mainline-style port at github.com/Bonstra/linux-ls1024a (branch
``ls1024a``, base v6.2.0, commit ``d751daba``).

Stage 1 goal (this repository's current state)
================================================

v6.18.46 builds cleanly for this board::

    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- ls1024a_defconfig
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage dtbs uImage

producing ``arch/arm/boot/uImage`` and
``arch/arm/boot/dts/nxp/ls/ls1024a-wdmycloud.dtb``. Booting on the
real board (serial console + SATA rootfs) has not been attempted
yet -- that is Stage 2.

What was ported, and from where
================================

Everything below was imported from Bonstra/linux-ls1024a@d751daba and,
where the v6.2 -> v6.18 kernel API had moved on, adapted to compile.
See the individual commit messages for the exact API changes fixed in
each file; the shared theme was ``platform_driver.remove()`` losing
its return value and going from ``int`` to ``void``.

- ``arch/arm/mach-ls1024a/`` -- machine init, SMP bring-up
- ``arch/arm/boot/dts/nxp/ls/ls1024a.dtsi``, ``-evm.dts``, ``-tsx31.dts``
- ``drivers/clk/clk-ls1024a.c``
- ``drivers/pinctrl/freescale/pinctrl-ls1024a.c``
- ``drivers/gpio/gpio-ls1024a.c``
- ``drivers/reset/reset-ls1024a.c``
- ``drivers/phy/freescale/phy-ls1024a-{serdes,usb3}.c``
- ``drivers/i2c/busses/i2c-ls1024a.c``
- ``drivers/watchdog/ls1024a_wdt.c``
- ``drivers/pci/controller/dwc/pcie-ls1024a.c``

UART, AHCI SATA, USB3 (dwc3), and SPI (dw_spi) use unmodified mainline
generic drivers -- no SoC-specific driver code was needed for those,
only device tree wiring.

``arch/arm/boot/dts/nxp/ls/ls1024a-wdmycloud.dts`` is new: a board
file for this specific product, based on ``ls1024a-evm.dts`` (the old
3.2.26 kernel identifies this machine as "Comcerto 2000 EVM", and the
peripheral MMIO addresses in a live boot log from this exact unit
match ``ls1024a.dtsi`` closely enough that it is almost certainly the
same reference design).

Known gaps -- not in scope for Stage 1
=======================================

Networking (PFE)
    The Comcerto Packet Forwarding Engine (EMAC1-3, EGPI1-3, HGPI,
    BMU1-2, CLASS, TMU, UTIL, HIF) is this SoC's entire network path
    and has no driver anywhere in mainline or in the Bonstra fork.
    The old 3.2.26 kernel's custom ``pfe`` module (source in
    symops/MCG1-3.2.26, ``kmodules/mspd-c2k/pfe``) is architecture
    documentation for a future port; the three firmware ELF blobs it
    loads (``class_c2000.elf``, ``tmu_c2000.elf``, ``util_c2000.elf``)
    are opaque microcode for the PFE's internal cores and should be
    reusable as-is regardless of kernel version, since they talk to
    the same hardware register/microcode ABI. This is expected to be
    the largest remaining piece of work by a wide margin.

RTC
    The board uses a proprietary "c2k-rtc" block (Mindspeed). No
    driver exists in mainline or in the Bonstra fork. Not wired into
    ``ls1024a-wdmycloud.dts``; the system currently has no persistent
    clock source and would need NTP after boot (as the old kernel's
    boot log shows: "Warning: Invalid RTC value so initializing it").

Board LEDs and fan
    ``drivers/leds/leds-wd.c`` and ``drivers/hwmon/wd-fan.c`` in the
    old 3.2.26 tree drive these directly through raw register writes
    rather than gpiolib:

    - ``system_led`` is a tri-color LED combining GPIO 5 (green), 6
      (blue), 7 (red) via ``COMCERTO_GPIO_OUTPUT_REG``, with an
      alternate PWM1/PWM2/PWM3 pulse mode selected through
      ``COMCERTO_GPIO_PIN_SELECT_REG``.
    - ``wifi_led`` combines GPIO 12/13 the same way.
    - the fan is driven purely through PWM0 duty cycle
      (``COMCERTO_LOW_DUTY_PWM0`` / ``COMCERTO_MAX_EN_PWM0``), not
      GPIO toggling.

    No LS1024A PWM driver exists yet in the Bonstra fork, and the RGB
    combining logic doesn't map cleanly onto the generic
    ``gpio-leds``/``pwm-fan`` bindings, so these were deliberately
    left out of the board DTS rather than describing hardware with no
    driver behind it.

PCIe
    ``pcie-ls1024a.c`` was ported and compiles (it turned out to need
    only one API fix -- ``dw_pcie_ops.link_up`` returning ``bool``
    instead of ``int`` -- the ``struct pcie_port`` -> ``struct
    dw_pcie_rp`` rename anticipated as the highest-risk item in the
    Stage 1 plan had already happened in the fork's own v6.2-era copy
    of this file). It is not wired into ``ls1024a-wdmycloud.dts``:
    the old kernel's boot log shows "PCIe0: Link Up Failed" with
    nothing physically connected on this board.

Power button
    "Button VAR: btn_status" appears in the barebox boot log and as a
    ``btn_status=`` kernel command-line argument, but no corresponding
    driver exists anywhere in the old 3.2.26 kernel tree -- it appears
    to be read and acted on by barebox/userspace scripts only, not the
    kernel. No ``gpio-keys`` node was added.

Toolchain note
==============

Unlike the 3.2.26 port (which needed a matched-era GCC 4.7 + binutils
2.22 toolchain to build at all), v6.18.46 builds cleanly with the
modern host toolchain (GCC 13, binutils 2.42) already present in this
environment -- no compiler-version workarounds were needed anywhere
in this port.
