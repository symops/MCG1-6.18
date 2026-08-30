# MCG1-6.18

Porting Linux **v6.18.46** to the WD My Cloud (Gen 1) NAS.

The stock kernel on this device is a vendor 3.2.26 build from 2012
(see [symops/MCG1-3.2.26](https://github.com/symops/MCG1-3.2.26)).
This repository tracks the effort to replace it with a current
long-term-support kernel, keeping the same (already modernized,
Devuan-based) rootfs.

## Hardware

- **Board:** WD My Cloud, Gen 1
- **SoC:** Mindspeed "Comcerto 2000", later renamed **LS1024A**
  after the Mindspeed -> MACOM -> NXP lineage. Dual-core ARM
  Cortex-A9 (ARMv7).
- No public mainline support for this SoC has ever existed. The
  closest available thing is a WIP out-of-tree port,
  [Bonstra/linux-ls1024a](https://github.com/Bonstra/linux-ls1024a)
  (branch `ls1024a`, based on v6.2.0, last updated 2023-03-11),
  written against the QNAP TS-x31 (a different product on the same
  SoC). This repository forward-ports that work to v6.18.46 and
  adapts it to the WD My Cloud gen1 board specifically.

## Status: Stage 1 -- it builds

```
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- ls1024a_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage dtbs uImage
```

produces a working `arch/arm/boot/uImage` (5.23 MiB) and
`arch/arm/boot/dts/nxp/ls/ls1024a-wdmycloud.dtb`, with
`Load Address = Entry Point = 0xF008000` -- the same value the
original 3.2.26 `uImage` for this exact board uses, so it should be a
drop-in replacement as far as barebox is concerned.

Real-hardware boot testing is under way (Stage 2), and has surfaced
two bootloader-level constraints so far:

1. barebox loads the kernel from a fixed 10 MiB raw region on disk
   (partitions 5/6) regardless of the uImage header's declared size,
   so the original 11.8 MiB image (a direct `multi_v7_defconfig`
   derivative, dozens of unrelated platforms built in) was silently
   truncated and failed its checksum. Fixed by trimming
   `ls1024a_defconfig` and switching the kernel's compressor from
   gzip to XZ -- down to 5.2 MiB.
2. This board's barebox (2011.06.0, Dec 2013) has no device-tree-aware
   `bootm` at all -- it only ever hands off via the old ATAG protocol,
   which a `DT_MACHINE_START`-only machine (ours) cannot match. Fixed
   with `CONFIG_ARM_APPENDED_DTB` + `CONFIG_ARM_ATAG_DTB_COMPAT`: the
   DTB is concatenated directly onto `zImage` at build time (not just
   `make uImage` -- see below), so the kernel finds it itself without
   any bootloader cooperation.

See `Documentation/arm/ls1024a-wdmycloud.rst` ("Kernel image size
budget" and "Boot protocol: appended DTB required") for the full
writeup of both, including the exact build command for the
DTB-appended image this board actually needs. Boot-to-shell over
serial has not been confirmed yet.

## What's in this repo

- One squashed baseline commit importing `linux-stable` v6.18.46
  (full upstream history intentionally not carried here to keep the
  repo a reasonable size -- it's on
  [git.kernel.org](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git),
  tag `v6.18.46`, for anyone who needs to trace a specific change).
- On top of that, one or two commits per subsystem, each importing
  the relevant file(s) from `Bonstra/linux-ls1024a@d751daba` and, if
  needed, a separate commit adapting them to the v6.18 kernel API:

  | Subsystem | Files |
  |---|---|
  | Machine / SMP | `arch/arm/mach-ls1024a/` |
  | Device tree | `arch/arm/boot/dts/nxp/ls/ls1024a{.dtsi,-evm.dts,-tsx31.dts}` |
  | Clock | `drivers/clk/clk-ls1024a.c` |
  | Pin control | `drivers/pinctrl/freescale/pinctrl-ls1024a.c` |
  | GPIO | `drivers/gpio/gpio-ls1024a.c` |
  | Reset controller | `drivers/reset/reset-ls1024a.c` |
  | SerDes / USB3 PHY | `drivers/phy/freescale/phy-ls1024a-{serdes,usb3}.c` |
  | I2C | `drivers/i2c/busses/i2c-ls1024a.c` |
  | Watchdog | `drivers/watchdog/ls1024a_wdt.c` |
  | PCIe | `drivers/pci/controller/dwc/pcie-ls1024a.c` |

  UART (`8250_dw`), AHCI SATA (`ahci_platform`) and SPI (`dw_spi`)
  needed no driver porting at all -- they're already generic mainline
  drivers, wired up purely through the device tree.

- **`arch/arm/boot/dts/nxp/ls/ls1024a-wdmycloud.dts`** -- new board
  file for this specific product, based on `ls1024a-evm.dts` (the old
  3.2.26 kernel identifies this machine as "Comcerto 2000 EVM", and
  the peripheral MMIO addresses captured from a live boot log on this
  exact unit match `ls1024a.dtsi` closely enough that it is almost
  certainly the same reference design). Enables UART1 (console), AHCI
  SATA, USB3 (dwc3 + PHY), both DesignWare SPI controllers, and I2C --
  everything confirmed present in that boot log.
- **`arch/arm/configs/ls1024a_defconfig`** -- the config this was all
  built and verified against.
- **`Documentation/arm/ls1024a-wdmycloud.rst`** -- longer-form porting
  notes: exact API changes fixed per file, and details on the gaps
  below.

## What's *not* ported yet

- **Networking (PFE).** The Comcerto Packet Forwarding Engine
  (EMAC1-3, EGPI1-3, HGPI, BMU1-2, CLASS, TMU, UTIL, HIF) is this
  SoC's entire network path, and has no driver anywhere in mainline
  or in the Bonstra fork. This is expected to be the largest
  remaining piece of work by a wide margin. The old kernel's custom
  `pfe` module (see `symops/MCG1-3.2.26`,
  `kmodules/mspd-c2k/pfe`) is the reference for a future port; its
  three firmware blobs (`class_c2000.elf`, `tmu_c2000.elf`,
  `util_c2000.elf`) are opaque microcode and should be reusable as-is
  regardless of kernel/driver version.
- **RTC.** Proprietary "c2k-rtc" block, no driver in mainline or in
  the fork. NTP-only timekeeping for now.
- **Board LEDs and fan.** Driven by the old kernel through raw
  register pokes (`drivers/leds/leds-wd.c`, `drivers/hwmon/wd-fan.c`
  in the 3.2.26 tree) rather than gpiolib/PWM frameworks, and there's
  no LS1024A PWM driver yet to build on. Left out of the board DTS
  rather than describing hardware nothing can drive.
- **PCIe**, though the driver compiles and is included: nothing is
  physically connected on this board (the old kernel's boot log shows
  "PCIe0: Link Up Failed"), so it's left disabled in the board DTS.

See `Documentation/arm/ls1024a-wdmycloud.rst` for the full detail on
each of these, including the exact register-level behavior of the
LED/fan code for whoever picks that up later.
