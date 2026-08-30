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

## Status: Stage 2 -- boots to a shell on real hardware

```
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- ls1024a_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- LOCALVERSION= zImage dtbs modules
cat arch/arm/boot/zImage arch/arm/boot/dts/nxp/ls/ls1024a-wdmycloud.dtb \
    > arch/arm/boot/zImage-w-dtb
mkimage -A arm -O linux -T kernel -C none -a 0x00008000 -e 0x00008000 \
    -n "Linux-6.18.46-ls1024a-wdmycloud" \
    -d arch/arm/boot/zImage-w-dtb arch/arm/boot/uImage
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- LOCALVERSION= \
    INSTALL_MOD_PATH=/some/staging/dir modules_install
```

`ls1024a_defconfig` sets `# CONFIG_LOCALVERSION_AUTO is not set`, which
drops the `-g<commit>` suffix from `uname -r`, but `scripts/setlocalversion`
still appends a bare `+` for an uncommitted/dirty tree unless the
`LOCALVERSION` make variable is explicitly passed (even empty, as
above) -- that's what makes the release string a clean `6.18.46`
matching the module directory (`/lib/modules/6.18.46/`) installed
above.

produces a `uImage` (~5.3 MiB) that, flashed to this board's kernel
partition, boots all the way to userspace: `md0` (the RAID1 rootfs)
assembles and mounts, sysvinit runs, udev populates `/dev`, both
filesystems get fscked, swap activates, cron/Dropbear SSH/MD
monitoring start -- the *same* Devuan rootfs the old 3.2.26 kernel
boots, now running under v6.18.46. (Plain `make uImage` still works
for a quick build check, but doesn't produce a bootable image for
this board on its own -- see below for why.)

Getting here surfaced a chain of board-specific fixes, each confirmed
against real hardware, in order:

1. barebox loads the kernel from a fixed 10 MiB raw region on disk
   (partitions 5/6) regardless of the uImage header's declared size,
   so the original 11.8 MiB image (a direct `multi_v7_defconfig`
   derivative, dozens of unrelated platforms built in) was silently
   truncated and failed its checksum. Fixed by trimming
   `ls1024a_defconfig` and switching the kernel's compressor from
   gzip to XZ.
2. This board's barebox (2011.06.0, Dec 2013) has no device-tree-aware
   `bootm` at all -- it only ever hands off via the old ATAG protocol,
   which a `DT_MACHINE_START`-only machine (ours) cannot match. Fixed
   with `CONFIG_ARM_APPENDED_DTB` + `CONFIG_ARM_ATAG_DTB_COMPAT`: the
   DTB is concatenated directly onto `zImage` at build time, so the
   kernel finds it itself without any bootloader cooperation.
3. `CONFIG_DEBUG_LL` + `CONFIG_EARLY_PRINTK` (UART1) added to get
   visibility into early boot at all -- essential for diagnosing the
   next two.
4. `arch/arm/mach-ls1024a/platsmp.c` wrote the Cortex-A9 secondary-CPU
   reset vector via `phys_to_virt(0)`, assuming physical address 0 is
   real RAM. It oopsed because the kernel's memory map didn't include
   physical address 0 at boot time -- see item 7 for why. Guarded with
   `memblock_is_memory()` as defense in depth; falls back to
   single-CPU boot instead of crashing if this ever recurs.
5. This SoC's AHCI HBA reads back `PORTS_IMPLEMENTED = 0` from
   hardware no matter how many ports exist (the old 3.2.26 driver
   forced this too) -- added `ports-implemented = <0x3>;` to the SATA
   DT node.
6. `ls1024a_defconfig` never built MD/RAID at all, and `root=/dev/md0`
   turns out to be genuinely correct (verified against a live boot log
   of the real Devuan install) -- enabled `CONFIG_MD`/`BLK_DEV_MD`/
   `MD_RAID1`.
7. Physical address 0 *is* real RAM on this board (the old kernel
   reports "Memory: 44MB 192MB = 236MB total" -- two banks, one
   starting near 0) and `CONFIG_ARM_ATAG_DTB_COMPAT` correctly imports
   both from barebox's ATAGs. But `CONFIG_AUTO_ZRELADDR` places the
   decompressed kernel (and hence `PHYS_OFFSET`) by rounding the
   *load* address down to a 128 MiB boundary -- with the old
   `LOADADDR=0x0F008000`, that lands on `0x08000000`, so the lower
   bank gets silently excluded from the kernel's own memory map (hence
   item 4's oops, and only ~128 MiB usable). Building with
   `LOADADDR=0x00008000` instead rounds to `0x0`, recovers the full
   ~236 MiB, and lets secondary-CPU bring-up work without needing
   item 4's guard to trigger at all. **Confirmed on hardware:** both
   CPU cores online, 235 MiB total memory.

See `Documentation/arm/ls1024a-wdmycloud.rst` for the full writeup of
each, plus what's still not working post-boot (networking, LEDs --
both already expected/tracked below).

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
- **`rsyslog: Permission denied` / `ntpsec: Permission denied`** at
  userspace startup -- seen on the real-hardware Stage 2 boot, not yet
  root-caused. Doesn't block reaching a shell.

See `Documentation/arm/ls1024a-wdmycloud.rst` for the full detail on
each of these, including the exact register-level behavior of the
LED/fan code for whoever picks that up later.
