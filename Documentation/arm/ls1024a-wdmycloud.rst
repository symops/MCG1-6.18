.. SPDX-License-Identifier: GPL-2.0

===========================================================
WD My Cloud (Gen 1) -- LS1024A/Comcerto 2000 port, Stage 1
===========================================================

This is a porting-progress note, not upstream-quality documentation.
It tracks the state of bringing v6.18.50 up on the WD My Cloud gen1
NAS (SoC: Mindspeed/Freescale "Comcerto 2000", later renamed
LS1024A after the Mindspeed/MACOM/NXP lineage), starting from a
working 3.2.26 vendor kernel (see symops/MCG1-3.2.26) and the WIP
mainline-style port at github.com/Bonstra/linux-ls1024a (branch
``ls1024a``, base v6.2.0, commit ``d751daba``).

Stage 1 goal (this repository's current state)
================================================

v6.18.50 builds cleanly for this board::

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
    clock source and needs NTP after boot (as the old kernel's boot
    log shows: "Warning: Invalid RTC value so initializing it") --
    confirmed on real hardware, boot always starts at a fixed
    build-date-ish time before NTP corrects it.

    This board has no RTC backup battery, so even a working c2k-rtc
    driver would not retain time across power loss -- NTP-after-boot
    is the correct permanent behavior here, not a stopgap. Writing
    this driver is low priority.

Board LEDs
    ``drivers/leds/leds-wd.c`` in the old 3.2.26 tree drives these
    directly through raw register writes rather than gpiolib:

    - ``system_led`` is a tri-color LED combining GPIO 5 (green), 6
      (blue), 7 (red) via ``COMCERTO_GPIO_OUTPUT_REG``, with an
      alternate PWM1/PWM2/PWM3 pulse mode selected through
      ``COMCERTO_GPIO_PIN_SELECT_REG``.
    - ``wifi_led`` combines GPIO 12/13 the same way.

    (The 3.2.26 tree also has ``drivers/hwmon/wd-fan.c``, PWM0-driven,
    but this specific board -- WD My Cloud Gen 1 -- has no physical
    fan; that driver is for a different SKU sharing the same vendor
    kernel image. Nothing to port here.)

    Plain on/off support (not the PWM pulse mode) is now wired up in
    ``ls1024a-wdmycloud.dts``: ``gpio-ls1024a.c`` already exposes GPIO
    lines 5-7 and 12-13 at the exact bit positions the vendor driver
    pokes, active-high, with no pinmux group needed (these pins
    default to GPIO mode -- only the PWM pulse mode switches them away
    from it, via ``COMCERTO_GPIO_PIN_SELECT_REG``, which this port
    doesn't do). ``system_led`` is three ``gpio-leds`` single-color
    LEDs (red/green/blue) combined via ``leds-group-multicolor``
    (``LED_COLOR_ID_RGB``) into one ``/sys/class/leds/system_led``
    with a ``multi_intensity`` channel per color. ``wifi_led`` is the
    same idea with two non-primary colors (yellow, blue) and
    ``LED_COLOR_ID_MULTI`` instead, matching the vendor driver's
    "white" state (both on at once). Not yet re-verified against real
    hardware as of this writing -- see the commit history for status.

    No LS1024A PWM driver exists yet in the Bonstra fork, so the
    breathing/pulse effect itself remains unported; only the generic
    software-side LED triggers (``linux,default-trigger`` etc.) are
    available for blinking for now.

PCIe
    ``pcie-ls1024a.c`` was ported and compiles (it turned out to need
    only one API fix -- ``dw_pcie_ops.link_up`` returning ``bool``
    instead of ``int`` -- the ``struct pcie_port`` -> ``struct
    dw_pcie_rp`` rename anticipated as the highest-risk item in the
    Stage 1 plan had already happened in the fork's own v6.2-era copy
    of this file). It is not wired into ``ls1024a-wdmycloud.dts``:
    the old kernel's boot log shows "PCIe0: Link Up Failed" with
    nothing physically connected on this board -- consistent with
    this SKU not exposing a usable PCIe slot/device at all, so this
    is treated as not applicable to this board rather than a pending
    port item.

Power button
    "Button VAR: btn_status" appears in the barebox boot log and as a
    ``btn_status=`` kernel command-line argument, but no corresponding
    driver exists anywhere in the old 3.2.26 kernel tree -- it appears
    to be read and acted on by barebox/userspace scripts only, not the
    kernel. No ``gpio-keys`` node was added.

Kernel image size budget (Stage 2 finding)
===========================================

barebox on this board loads the kernel from a fixed-size raw region
on the SATA disk (partitions 5/6), not a filesystem: its boot script
reads a **fixed 20480 sectors (10 MiB)** into RAM and only then
runs the uImage header's data-CRC check. The image is not sized from
the uImage header before the read -- if the actual ``uImage`` is
larger than 10 MiB, the read is silently truncated and barebox fails
with ``Verifying Checksum ... Bad Data CRC``. This was hit on first
real-hardware boot attempt: the initial ``ls1024a_defconfig`` (a
direct ``multi_v7_defconfig`` derivative, i.e. dozens of unrelated
SoC platforms and their drivers built in alongside LS1024A) produced
an 11.8 MiB ``uImage``.

Fix, in two parts:

1. Disabled every ``CONFIG_ARCH_*`` platform switch other than the
   multiplatform/multi-v7 plumbing and ``ARCH_LS1024A`` itself, plus
   the built-in (non-modular) subsystems with no use on this board:
   ``NETDEVICES`` (no net driver exists for this SoC yet -- see PFE
   above), ``DRM``, ``USB_GADGET``, ``MMC``, ``INPUT_TOUCHSCREEN``,
   ``WIRELESS``, and ``DEBUG_INFO``. This is a one-way ``olddefconfig``
   cascade -- Kconfig drops every driver that ``depends on`` a
   disabled platform automatically.
2. Switched the kernel's self-decompressing payload from
   ``CONFIG_KERNEL_GZIP`` to ``CONFIG_KERNEL_XZ``. Note this is
   independent of barebox: ``make uImage`` on ARM always builds the
   mkimage header with ``-C none`` (see ``scripts/Makefile.lib``) --
   the zImage payload decompresses itself, at boot, using the
   decompressor code linked into ``arch/arm/boot/compressed/``. The
   bootloader never parses the compression format at all, so
   barebox's own age/feature set is irrelevant to this choice.

Result: ``uImage`` dropped from 11.8 MiB to **5.23 MiB**, comfortably
under the 10 MiB budget with room for future growth (a PFE driver,
once ported, will add back some size). Anyone adding drivers to
``ls1024a_defconfig`` going forward should keep an eye on
``ls-la arch/arm/boot/uImage`` against this 10 MiB ceiling.

Boot protocol: appended DTB required (Stage 2 finding)
========================================================

The barebox on this board (2011.06.0-svn10510, Dec 2013 build) has
**no device-tree-aware `bootm`** -- confirmed on real hardware via
``bootm -h``, which only lists ``-r <initrd>``, ``-a <arch>``,
``-R <system_rev>``. Its stored boot script (``/env/bin/boot_sata``,
and the near-identical one actually run from the SATA disk's
partition 7 via ``sataenv run 7``) does a plain::

    satapart 0x3008000 5 0x5000   # read partition 5 -> RAM
    bootm /dev/mem.uImage         # no dtb, ever

This is a legacy ATAG-only handoff: barebox builds a real ATAG list
(ATAG_CORE + cmdline, confirmed by dumping the memory at the ``r2``
address from the kernel's own error output) and passes it the way it
always has for the old 3.2.26 board-file kernel. But
``arch/arm/mach-ls1024a/ls1024a.c`` is ``DT_MACHINE_START``-only --
matched purely by the FDT's ``compatible`` string -- so it cannot be
reached via ATAGs alone, no matter what machine/arch number is
passed. First real-hardware boot attempt hit exactly this, printing
(thanks to ``CONFIG_DEBUG_LL``, added specifically to surface this)::

    Error: invalid dtb and unrecognized/unsupported machine ID
      r1=0x00000446, r2=0x00000100
    Available machine support:
    ID (hex)        NAME
    ffffffff        Generic DT based system
    ffffffff        Freescale LS1024A
    ...

Upgrading barebox itself is out of scope -- the fix is the standard
one for exactly this situation: ``CONFIG_ARM_APPENDED_DTB`` (kernel
looks for a DTB concatenated directly after its own zImage payload
when no valid FDT pointer arrives via ``r2``) plus
``CONFIG_ARM_ATAG_DTB_COMPAT`` (imports the bootloader's real ATAG
list -- memory banks, cmdline -- into that appended DTB at boot).
Both were already implicitly enabled (inherited from
``multi_v7_defconfig``) -- the missing piece was simply that
``make uImage`` never concatenates a DTB on its own. The actual fix
is a build-time step::

    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- ls1024a_defconfig
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage dtbs
    cat arch/arm/boot/zImage \
        arch/arm/boot/dts/nxp/ls/ls1024a-wdmycloud.dtb \
        > arch/arm/boot/zImage-w-dtb
    mkimage -A arm -O linux -T kernel -C none \
        -a 0x00008000 -e 0x00008000 \
        -n "Linux-6.18.50-ls1024a-wdmycloud" \
        -d arch/arm/boot/zImage-w-dtb arch/arm/boot/uImage

(Load address history: earlier builds used ``0x0F008000`` -- the
address the original 3.2.26 ``uImage`` was built with -- on the
assumption that matching it made no difference for a DT kernel. It
does: see "Second CPU core / missing 128 MiB of RAM" below for why
that address was actually wrong for this kernel and had to change.)

(plain ``make uImage`` still works and stays useful for a quick build
sanity check -- it just isn't the artifact to flash on this board.)
Resulting image: 5.24 MiB, still comfortably under the 10 MiB budget
from the section above.

The second real-boot attempt with this appended-DTB image produced
**no output at all** after barebox's own ``arch_number: 1094`` line --
notably, *not* the "unrecognized machine ID" error from the first
attempt. Since that error is printed by the same low-level
``CONFIG_DEBUG_LL`` path in both cases, its disappearance is a strong
signal the machine lookup now succeeds (DTB found, ``LS1024A``
matched) and boot proceeds into ``start_kernel()`` -- just silently,
because the only command line in play was barebox's own ATAG cmdline
(``CONFIG_ARM_ATAG_DTB_COMPAT_CMDLINE_FROM_BOOTLOADER``), which has no
``earlyprintk``, so nothing reaches the console until the real UART
platform driver probes -- much later in boot, and not guaranteed to
happen at all if something upstream of it (clk/pinctrl) is wrong.

Fixed by switching to ``CONFIG_ARM_ATAG_DTB_COMPAT_CMDLINE_EXTEND``
(so the DTB's own ``bootargs``, which already had ``earlyprintk``, are
kept and barebox's real per-unit args get appended after) and adding
``CONFIG_EARLY_PRINTK=y`` (reuses the already-verified ``DEBUG_LL``
UART1 code path for a console active from very early boot, long
before the platform driver probes).

Third real-hardware attempt (with both fixes applied) confirmed the
appended-DTB boot protocol works end to end: the kernel decompresses,
matches the DT machine (``OF: fdt: Machine model: Western Digital My
Cloud (Gen 1)``), and runs through RCU/SLUB/scheduler init with full
console output. It then oopsed and panicked in
``kernel_init_freeable`` -> ``ls1024a_smp_prepare_cpus``::

    Unable to handle kernel paging request at virtual address b8000000 when write
    Register r5 information: 0-page vmalloc region ... allocated at ls1024a_smp_prepare_cpus+0x24/0xd0

``arch/arm/mach-ls1024a/platsmp.c`` writes the Cortex-A9 secondary-CPU
reset vector via ``vectors_base = phys_to_virt(CPU_VECTORS_PHYS)``
with ``CPU_VECTORS_PHYS = 0x0``, assuming physical address 0 is
backed by real, linearly-mapped RAM (true on the board the Bonstra
fork was written against). On this board RAM starts at
``0x08000000`` (confirmed by the kernel's own
``Early memory node ranges: [mem 0x08000000-0x0fffffff]``, and by
``OF: fdt: Ignoring memory range 0x0 - 0x8000000`` earlier in the same
boot), so ``phys_to_virt(0)`` computes a bogus, unmapped virtual
address -- and the arithmetic confirms it exactly:
``0x0 - 0x08000000 + PAGE_OFFSET(0xC0000000) = 0xB8000000``, precisely
the faulting address. The existing ``if (!vectors_base)`` guard never
catches this because ``phys_to_virt()`` is pure arithmetic and can't
return NULL.

Fixed with a ``memblock_is_memory(CPU_VECTORS_PHYS)`` check before
using the computed pointer: if physical address 0 isn't real RAM on
this board, secondary-CPU bring-up is skipped (single-CPU boot)
instead of crashing the kernel.

Fourth attempt (with the SMP fix) got much further: full console
output through driver probing, no oops. It stopped at::

    ahci 9d000000.sata: masking port_map 0x0 -> 0x0
    ahci 9d000000.sata: 0/2 ports implemented (port mask 0x0)
    ...
    VFS: Cannot open root device "/dev/md0" or unknown-block(0,0): error -6
    Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)

The AHCI HBA on this SoC reads back ``PORTS_IMPL = 0`` from hardware
regardless of how many ports actually exist -- the same issue the old
3.2.26 vendor driver worked around by unconditionally
"forcing PORTS_IMPL to 0x3" (visible in its own boot log). The
mainline ``generic-ahci``/``libahci_platform`` driver has a standard
DT property for exactly this, ``ports-implemented``, which forces
``hpriv->saved_port_map`` and overrides whatever the hardware register
says (see ``libahci.c:ahci_save_initial_config()``). Added
``ports-implemented = <0x3>;`` to the ``sata@9d000000`` node in
``ls1024a.dtsi``.

``root=/dev/md0`` will still fail (MD/RAID support isn't built in
this defconfig) -- that's expected and separate from the AHCI fix;
the plan is to see the real partition table once SATA actually
enumerates a disk, and either build a `dm`-free multi-boot ``root=``
against the correct real partition, or enable MD if the on-disk
layout turns out to need it.

Fifth attempt (with the AHCI fix) confirmed the disk and full
partition table now enumerate correctly::

    ahci 9d000000.sata: 2/2 ports implemented (port mask 0x3)
    sd 0:0:0:0: [sda] 976773168 512-byte logical blocks: (500 GB/466 GiB)
     sda: sda1 sda2 sda3 sda4 sda5 sda6 sda7 sda8

Cross-checked against a live boot log of the *real* Devuan install on
this exact disk (``mcg1-devuan.log``): ``root=/dev/md0`` is not stale
leftover from the old kernel's stored ATAG cmdline -- it is genuinely
correct and current. ``md0`` is a RAID1 mirror of ``sda1``+``sda2``
(~20 GiB each) holding the real ext3 rootfs (confirmed:
``md: created md0`` / ``md/raid1:md0: active with 2 out of 2
mirrors`` / ``VFS: Mounted root (ext3 filesystem)``); ``sda3`` is
swap; ``sda4`` (~421 GiB) is the big data volume; ``sda5``/``sda6``
(~95/96 MiB) are the two kernel slots this port has been using all
along; ``sda7``/``sda8`` (1/2 MiB) hold the barebox boot scripts.

The only actual gap was that this defconfig never built MD/RAID
support at all. Fixed by enabling ``CONFIG_MD``, ``CONFIG_BLK_DEV_MD``
and ``CONFIG_MD_RAID1`` (built-in, not modules -- required for root
autodetection to work with ``noinitrd``, matching
``raid=autodetect`` in the passed cmdline).

Stage 2 reached: boots to userspace on real hardware
=======================================================

Sixth real-hardware attempt (with the MD/RAID1 fix) is the first full
success: ``md0`` auto-assembles from ``sda1``+``sda2``, mounts as the
real ext3 root filesystem, ``/sbin/init`` (sysvinit 3.14) runs,
udev/eudev populates ``/dev``, both filesystems (``md0`` and the
``sda4`` data volume) get fscked and mounted, swap activates, and the
existing Devuan userspace comes up: cron, Dropbear SSH (generates and
installs all three host keys and restarts successfully), MD
monitoring. This is the same rootfs the old 3.2.26 kernel booted
(``mcg1-devuan.log``), now running unmodified under v6.18.50.

Remaining issues visible in this boot, all expected and separate from
the kernel-boot work above:

- ``modprobe: FATAL: Module pfe not found`` / ``Cannot find device
  "eth0"`` / DHCP failure -- the PFE network driver doesn't exist yet
  (see "Known gaps" above; this is the big Stage 3 item).
- ``/etc/init.d/wd-leds: line 16: ... No such file or directory`` --
  no LS1024A PWM/LED driver yet (also already listed above).
- ``Checking root file system...Cannot persist the following output
  on disc ... failed!`` (fsck's own diagnostic banner, filesystem
  itself reports clean either time) and an ``/etc/mtab`` symlink
  warning -- cosmetic, not investigated yet.
- Only CPU0 is online (``htop`` shows CPU1 permanently offline). See
  next section -- root-caused and fixed.

Second CPU core / missing 128 MiB of RAM
===========================================

The oops from the fourth attempt (see the SMP-guard section above)
was patched by skipping secondary-CPU bring-up whenever
``CPU_VECTORS_PHYS`` (physical address 0) isn't backed by RAM. That
made the kernel stop crashing, but it begged the question: *why*
isn't physical address 0 real RAM here, when the old 3.2.26 vendor
driver (``arch/arm/mach-comcerto/platsmp.c``, ``boot_secondary()``)
writes CPU1's reset vector to that exact same address and it works
fine on this same board? Checking the old kernel's own boot log
answers it directly::

    [    0.000000] Memory: 44MB 192MB = 236MB total

Two banks. Physical address 0 *is* real, populated RAM -- roughly a
44 MiB bank starting near 0, plus a ~192 MiB bank higher up. Our
kernel's own boot log shows only the upper bank::

    OF: fdt: Ignoring memory range 0x0 - 0x8000000
    Early memory node ranges
      node   0: [mem 0x0000000008000000-0x000000000fffffff]

Tracing why: ``CONFIG_ARM_ATAG_DTB_COMPAT`` imports memory banks from
barebox's real ATAGs into the appended DTB at boot time
(``arch/arm/boot/compressed/atags_to_fdt.c``) -- that part is working
correctly and *is* passing both banks through. The drop happens later,
in ``drivers/of/fdt.c:early_init_dt_add_memory_arch()``, which clips
any bank starting below ``MIN_MEMBLOCK_ADDR`` (== ``PHYS_OFFSET``) --
and ``PHYS_OFFSET`` here is not derived from the memory banks at all.
It's set in ``arch/arm/kernel/head.S`` as ``__pa(_text)``: wherever the
*decompressed* kernel physically ends up running from.

That placement, in turn, is decided earlier still, in the
self-decompressing stub (``arch/arm/boot/compressed/head.S``, guarded
by ``CONFIG_AUTO_ZRELADDR=y``, which this defconfig has)::

    mov  r0, pc
    and  r0, r0, #0xf8000000   @ round down to a 128 MiB boundary

i.e. the decompressor takes wherever it's currently executing from and
rounds down to the nearest 128 MiB boundary -- that becomes the
kernel's final physical home, independent of where the *compressed*
zImage payload itself was loaded, *except* for which 128 MiB window
that load address falls into. Builds up to this point used
``LOADADDR=0x0F008000`` (matching the old 3.2.26 image, on the
assumption it wouldn't matter for a DT kernel). ``0x0F008000 &
0xf8000000 = 0x08000000`` -- exactly the cutoff seen in the "Ignoring
memory range" line above. The kernel was quite literally deciding to
live in the *upper* bank and, by that same stroke, permanently
excluding the lower one from its own linear memory map -- which is
also why ``memblock_is_memory(0)`` correctly reported "no" and the
SMP guard correctly (if only symptomatically) kicked in.

Fix: build with ``LOADADDR=0x00008000`` (and matching ``-e``) instead
-- see the appended-DTB build command above. ``0x00008000 &
0xf8000000 = 0x0``, so the kernel settles at the true start of RAM,
both banks stay in the memory map, ``memblock_is_memory(0)`` now
correctly returns true, and the original (unguarded) vector-write
path in ``platsmp.c`` -- the same one the 3.2.26 driver uses -- works
without needing its own special case. Also recovers the missing ~128
MiB of RAM (matches the old kernel's "236 MB total" report much more
closely than the ~128 MiB this kernel could see before). The
``memblock_is_memory()`` guard in ``platsmp.c`` is left in place as a
defensive check -- it's a no-op once the memory map is correct, and a
useful safety net if a future defconfig change reintroduces the
wrong-bank scenario.

Confirmed on real hardware::

    root@MCG1-Devuan:~# cat /proc/cpuinfo
    processor       : 0
    ...
    processor       : 1
    ...
    root@MCG1-Devuan:~# free -m
                   total        used        free      shared  buff/cache   available
    Mem:             235          40          81           0         121         194

Both CPU cores online, 235 MiB total memory (matches the old kernel's
"236MB total" almost exactly). Both the SMP and the memory-size issue
are resolved by this one build-time change.

None of these block reaching a working shell. Stage 2 (as scoped) is
done: this kernel boots the real rootfs on the real board over
serial.

SPI-NOR boot flash access from Linux
=======================================

barebox itself lives on, and stores its environment on, a SPI-NOR
flash chip -- visible in its own boot log ("``c2k_spi_probe``",
"``Using ENV from SPI Flash``") but never exposed to Linux in any of
the vendor's board files (``board-c2kasic.c``'s ``spi_board_info``
table only describes VoIP-reference-design peripherals -- ``proslic``,
``legerity`` -- that don't exist on this product; it looks like an
unmodified copy of Mindspeed's generic reference design).

Identified via barebox's own ``devinfo`` on real hardware: the chip is
attached as ``S25FL064A0`` under ``c2k_spi0`` (the *low-speed*
DesignWare SPI controller -- ``c2k_fast_spi`` is a separate driver in
the same listing, confirming this is ``ls_spi``
(``spi@90498000``), not ``hs_spi``). ``S25FL064A`` is a Spansion/Cypress
8 MiB SPI-NOR part old enough to predate SFDP; it matches upstream
``drivers/mtd/spi-nor/spansion.c``'s ``"s25sl064a"`` entry exactly
(3-byte JEDEC ID ``01 02 16``). Added as a child of ``&ls_spi`` in
``ls1024a-wdmycloud.dts``::

    &ls_spi {
        status = "okay";
        flash@0 {
            compatible = "spansion,s25sl064a", "jedec,spi-nor";
            reg = <0>;
            spi-max-frequency = <20000000>;
        };
    };

``CONFIG_MTD``, ``CONFIG_MTD_SPI_NOR``, ``CONFIG_MTD_BLOCK`` and
``CONFIG_SPI_DESIGNWARE`` were already enabled in ``ls1024a_defconfig``
(inherited from ``multi_v7_defconfig``), so no config change was
needed -- should show up as ``/dev/mtd0`` once booted. ``dtc`` prints
two harmless ``spi_bus_bridge``/``spi_bus_reg`` warnings when building
this dtb; they're a false positive from an unrelated pinctrl pin-group
subnode in ``ls1024a.dtsi`` that happens to be named ``spi`` (not an
actual SPI bus), tripping dtc's name-based heuristic -- unrelated to
this flash node, which compiles into the dtb correctly (verified with
``dtc -I dtb -O dts``). Not yet tested on hardware.

**Caution:** this is the same chip barebox boots from and stores its
environment on. Read access (dumping/verifying) is safe; before
writing anything back to it, back up the existing contents first
(``dd if=/dev/mtd0 of=backup.bin``) -- a bad write here risks the
bootloader itself, unlike the kernel-partition mistakes elsewhere in
this project which just needed a reflash.

First real-hardware attempt with this node **hung solid** right after
``dw_spi_mmio_driver_init`` / ``spi-nor spi0.0: supply vcc not found,
using dummy regulator`` -- no oops, no panic, nothing further at all;
reproduced identically (same message, same point) on a second boot,
both times requiring a manual power cycle. Root cause: neither
``&ls_spi`` nor ``&hs_spi`` in ``ls1024a.dtsi`` set ``pinctrl-0`` at
all, even though the dtsi defines the needed groups
(``pinctrl_spi``/``pinctrl_spi_ss0..3``, explicitly commented "Low
speed SPI"). Console/UART happens to keep working without this
because barebox already left the UART pins correctly muxed from its
own use of them; SPI has no such luck, and the DW SPI controller's
transfer-complete wait spins forever with its clock/pins never
actually configured for the SPI function.

The sibling ``ls1024a-tsx31.dts`` (same fork, different board) already
solved exactly this for its own SPI-NOR flash on this same
controller, including a second, separate issue: this DW SPI IP's
native chip-select releases as soon as the (only 8-word) TX FIFO
drains, cutting SPI-NOR command sequences short, worked around there
with a GPIO-driven ``cs-gpios`` instead of the controller's own CS
line. Copied its ``pinctrl-0``, ``num-cs``, and SPI mode/frequency
(4 MHz, mode 3 -- also matches ``SPI_MODE_3`` used throughout the
vendor 3.2.26 board file) onto our flash node. **Did not** copy its
``cs-gpios = <&gpio 18 ...>`` -- GPIO 18 is tsx31-specific board
wiring, unverified for this board, and guessing wrong there risks
reintroducing a hang for a different reason. If the flash now probes
without hanging but reports a wrong/garbled ID or read errors (not a
hang -- a normal probe-failure message), that is almost certainly the
missing GPIO-CS workaround, and finding this board's real CS GPIO is
the next step.

The pinctrl fix above was **not sufficient** -- second real-hardware
attempt hung identically, same point, same message, twice in a row.
Real root cause found by reading ``drivers/spi/spi-dw-core.c``'s own
source comment on ``dw_spi_poll_transfer()``::

    Note this method the same way as the IRQ-based transfer won't work
    well for the SPI devices connected to the controller with native CS
    due to the automatic CS assertion/de-assertion.

That is precisely tsx31's "shallow FIFO releases CS too early" problem
restated from the driver's own side, and it's *unconditional for
IRQ-driven transfers*, independent of pinctrl. ``ls_spi`` has an
``interrupts`` property, so ``spi-dw-mmio.c`` always took the
interrupt-driven path (``dws->irq != IRQ_NOTCONNECTED``) rather than
the polling path that comment recommends -- and that MMIO glue driver
treats ``platform_get_irq()`` as mandatory, with no way to opt into
polling via DT as-is.

Cross-checked against WD's own bootloader source (also GPL, bundled
alongside the kernel source as ``barebox-2011.06.0``):
``arch/arm/boards/comcerto-asic/c2k_asic.c`` names this exact chip
("S25FL064A", chip_select 0, mode ``SPI_CPOL | SPI_CPHA``, 4 MHz --
matching what's already in our flash node) driven by
``drivers/spi/c2k_spi.c``, a Mindspeed-proprietary, **polling-only**
driver with no GPIO chip-select anywhere in it (chip select is a pure
register write, ``ser = 1 << chip_select``). This confirms natively:
polling + native CS is not a workaround, it's what this hardware
actually wants -- barebox has been doing exactly that successfully on
every single boot in this whole project.

Fix: made the interrupt genuinely optional in ``spi-dw-mmio.c``
(``platform_get_irq_optional()``, falling back to
``IRQ_NOTCONNECTED`` -- the same value ``drivers/spi/spi-dw-bt1.c``
hardcodes unconditionally for a different SoC with this same
limitation, just made DT-selectable here instead of always-off), then
``/delete-property/ interrupts;`` on ``&ls_spi`` in
``ls1024a-wdmycloud.dts`` only -- other boards/nodes using this driver
keep interrupt-driven behavior unaffected. Confirmed in the compiled
dtb (``fdtget ... interrupts`` now reports ``FDT_ERR_NOTFOUND``, as
intended).

Third real-hardware attempt (with the polling fix) hung **identically**
-- same message, same point, twice in a row -- proving the IRQ/native-CS
theory, while real, wasn't the (or the whole) cause. Re-derived the
register-level pin assignments from the vendor 3.2.26 Linux source
(``arch/arm/mach-comcerto/include/mach/comcerto-2000/gpio.h``:
``SPI_MUX_BUS_1``/``SPI_MUX_BUS_2``, confirming SCLK=GPIO31,
TXD=GPIO30, SS0=GPIO18, RXD=GPIO32 -- note RXD alone lives in the
*other* 32-bit GPIO bank, ``COMCERTO_GPIO_63_32_PIN_SELECT`` @ 0xdc,
not ``COMCERTO_GPIO_PIN_SELECT_REG1`` @ 0x5c like the rest) and
compared it line-by-line against ``pinctrl-ls1024a.c``'s
``ls1024a_pmx_set_group_mux()``: register offsets match exactly
(``GPIO_PIN_SELECT_REG1 = 0x5c``, ``GPIO_63_32_PIN_SELECT = 0xdc``),
the GPIO1-bank pin (``spi_rxd``, mux_idx 0) is handled through a
correctly distinct code path, and the vendor header's actual per-pin
mux values (``GPIO18_SPI_SS0_N``, ``GPIO31_SPI_SCLK``,
``GPIO32_SPI_RXD``, etc.) are *all* literally ``(0x0 << shift)`` --
matching this driver's uniform ``spi`` function mux_value of 0
everywhere. No discrepancy found by inspection; the pinctrl driver
appears to correctly implement what the vendor's own register map
says it should.

With no further bug found by reading code, added a bounded timeout (1
second) and raw register dump (``SR``/``RISR``/``TXFLR``/``RXFLR``/
``SSIENR``) to ``dw_spi_poll_transfer()`` in ``spi-dw-core.c`` in
place of its unbounded ``while (dws->rx_len)`` loop -- this doesn't
fix anything by itself, but turns the indefinite hang into a bounded
failure (boot continues, no more power cycles needed to recover) and
should print the actual hardware register state at the point of
failure on the next attempt, which is needed to make further progress
here without guessing.

Fourth attempt (with the timeout) is the most informative result yet,
precisely because the timeout **never fired**: no diagnostic message,
identical hang, identical point, twice more. A jiffies-based software
timeout cannot fire if the CPU itself never retires the instruction
it's checking after -- which is exactly what an MMIO *read* to a
peripheral whose bus transaction never gets a response does on ARM
(unlike a write, which is typically posted). This reframes the whole
problem: not a driver logic bug, but the SPI block's registers being
genuinely unreachable over the bus -- most consistent with the block
still being held in hardware reset.

Checked ``include/dt-bindings/reset/ls1024a.h`` for a reset line
matching ``ls_spi``'s clock domain (``"dus"``, ``LS1024A_CLK_DUS`` --
shared with ``uart1``) and found ``LS1024A_AXI_DUS_RST``, previously
unused anywhere in this tree. ``drivers/spi/spi-dw-mmio.c`` already
looks up and deasserts an *optional* reset named ``"spi"``
(``devm_reset_control_get_optional_exclusive(&pdev->dev, "spi")``,
``reset_control_deassert()`` right after) -- it was simply never given
one, so this step silently no-ops and the driver proceeds to touch
registers of a block that may never have come out of reset. Added::

    resets = <&clkreset LS1024A_AXI_DUS_RST>;
    reset-names = "spi";

to ``&ls_spi``. Confirmed in the compiled dtb (``fdtget ...
resets`` -> ``4 672`` -- 672 decimal = 0x2a0 =
``LS1024A_AXI_DUS_RST``, ``reset-names`` -> ``"spi"``).

Sharing a clock gate with a working peripheral (uart1) doesn't
necessarily mean sharing a reset bit -- reset and clock gating are
independent hardware concerns, uart1 may have its own separate reset
(or barebox may simply leave it deasserted already for its own console
use, the same way it leaves uart1's pinmux correctly configured
without Linux ever touching pinctrl for it either).

Fifth attempt (with the reset fix) hung identically again. Before
trying another blind DT change, got ground truth directly from
barebox -- which is, after all, already reading/writing this exact
chip for its own environment every single boot::

    Barebox-C2K >/ crc32 -f /dev/spi0 0+0x100
    CRC32 for /dev/spi0 0x00000000 ... 0x000000ff ==> 0xd3d38ff3

barebox reads 256 real bytes from the chip successfully, moments
before jumping to Linux, in the exact same boot session that then
hangs. This conclusively rules out a hardware/wiring/silicon fault --
the chip, bus, and pin wiring are demonstrably fine right up until
Linux's own driver touches them. The bug is entirely in Linux-side
configuration or driver behavior.

Since the earlier 1-second timeout in ``dw_spi_poll_transfer()``
never fired (proving the hang is a blocking MMIO *read* instruction,
not a loop), added fine-grained ``dev_info()`` tracing at every step
of ``dw_spi_transfer_one()`` and each write/delay/read/status-check
sub-step inside ``dw_spi_poll_transfer()``'s loop. Prints emitted
*before* the hanging instruction will have already reached the
console (UART output isn't blocked by a stuck SPI MMIO read on the
same core -- the two are independent bus targets), so whichever trace
line is the *last* one printed on the next attempt pinpoints the exact
register access that never returns.

Sixth attempt (with the tracing): none of the added trace lines
printed at all -- not even ``transfer_one: entry``. This means the
hang happens in a *different* code path entirely, one that never
calls ``dw_spi_transfer_one()``.

Found it: ``dw_spi_init_mem_ops()`` registers a ``spi_mem`` layer
``exec_op`` callback (``dw_spi_exec_mem_op()``) whenever
``!dws->set_cs`` -- i.e. exactly the *native-CS, no GPIO override*
configuration this board uses. Its own comment explains why it exists::

    The SPI memory operation implementation below is the best choice
    for the devices, which are selected by the native chip-select
    lane. It's specifically developed to workaround the problem with
    automatic chip-select lane toggle when there is no data in the Tx
    FIFO buffer.

This is the driver's *actual*, already-correct answer to the whole
native-CS/shallow-FIFO problem from ``tsx31.dts`` -- it's just a
completely separate function from ``dw_spi_transfer_one()``/
``dw_spi_poll_transfer()``, which spi-nor's core prefers whenever
available, bypassing the traced path entirely (hence zero trace
output).

Inside it, ``dw_spi_write_then_read()`` has this, completely
unguarded::

    len = dws->rx_len;
    buf = dws->rx;
    while (len) {
        entries = readl_relaxed(dws->regs + DW_SPI_RXFLR);
        if (!entries) {
            sts = readl_relaxed(dws->regs + DW_SPI_RISR);
            if (sts & DW_SPI_INT_RXOI) { ...; return -EIO; }
            continue;
        }
        ...
    }

A tight software spin loop with no bound at all -- not the earlier
timeout-proof MMIO-read-stall theory; this is a genuine infinite
``while (len)`` if the Rx FIFO level register never reports any
entries. This is also finally consistent with barebox's own successful
``crc32`` read moments earlier: the hardware works, but this specific
mainline code path apparently never sees any Rx data arrive on this
controller.

Added the same timeout+register-dump pattern used before, but this
time in the actually-executing function -- both the Tx and Rx
``while`` loops in ``dw_spi_write_then_read()``, plus entry/milestone
``dev_info()`` calls throughout.

Seventh attempt: no output from any of the ``wtr:``/``exec_mem_op:``
tracing either. Since ``dw_spi_exec_mem_op()`` itself (before ever
calling ``dw_spi_write_then_read()``) had no tracing yet, added it
there too, plus the ``spi_mem_op`` fields (opcode, address, dummy
cycles, data direction/length) at entry.

Eighth attempt is the most informative one yet -- full, real trace
output, right up to::

    exec_mem_op: enter opcode=0x9f addr.nbytes=0 dummy.nbytes=0 data.dir=1 data.nbytes=6 max_freq=4000000
    exec_mem_op: init_mem_buf done, tx_len=1 rx_len=6
    exec_mem_op: cfg tmode=3 freq=4000000 max_mem_freq=200000000
    exec_mem_op: enable_chip(0) done, SSIENR=0x0
    exec_mem_op: update_config done, current_freq=4000000
    exec_mem_op: mask_intr done
    exec_mem_op: enable_chip(1) done, SSIENR=0x1 SER=0x0 SR=0x6
    wtr: enter tx_len=1 rx_len=6 fifo_len=8 SR=0x6 SSIENR=0x1 SER=0x0
    wtr: prefilled, SR=0x2 TXFLR=0x1
    wtr: CS asserted, remaining tx len=0 SR=0x2 SER=0x1 TXFLR=0x1
    wtr: Tx done, rx_len=6 SR=0xe RXFLR=0x1

This is opcode ``0x9F`` (JEDEC READ ID), no address/dummy bytes, 6
data bytes expected. Every setup step succeeds; CS asserts (``SER``
goes 0x0 -> 0x1); the single opcode byte transmits (``TXFLR`` empties,
``SR`` bit2/TFE sets); and -- critically -- ``SR=0xe`` at "Tx done"
already has bit3 (RFNE, Rx FIFO Not Empty) set with ``RXFLR=0x1``: one
byte had *already* arrived. The hardware is unambiguously alive and
transacting. Then: nothing further at all, not even the Rx loop's own
timeout/overflow error paths, which should have fired within a
second.

That last point turned out to be the real bug -- in the diagnostic
code, not the hardware. ``dw_spi_exec_mem_op()`` wraps the call to
``dw_spi_write_then_read()`` in ``local_irq_save()`` +
``preempt_disable()`` (deliberately, to keep the CS-atomic transfer
from being preempted -- see that function's own comment). ``jiffies``
is incremented by the periodic timer *interrupt*; with interrupts
disabled for the whole call, it cannot advance, so a deadline computed
as ``jiffies + HZ`` at loop entry can never be reached --
``time_after(jiffies, deadline)`` was silently, permanently false the
entire time. Both timeouts added in the last two commits (here and in
``dw_spi_poll_transfer()``) were dead code for this reason -- not
proof of anything about the hardware.

Replaced all three jiffies-based deadlines with plain loop-iteration
counters (which don't care whether interrupts are enabled), and added
a periodic ("every ~1M iterations") progress print to the Rx loop
specifically, since that's where execution stops. This should finally
produce a real timeout/diagnostic on the next attempt instead of
silently spinning past a check that can never trip.

Ninth attempt confirmed it, with a real, working timeout at last::

    wtr: Tx done, rx_len=6 SR=0xe RXFLR=0x1
    wtr: Rx loop still waiting, iters=1048576 remaining=5 SR=0x6 RISR=0x1 TXFLR=0x0 RXFLR=0x0
    wtr: Rx loop still waiting, iters=2097152 remaining=5 SR=0x6 RISR=0x1 TXFLR=0x0 RXFLR=0x0
    wtr: Rx loop still waiting, iters=3145728 remaining=5 SR=0x6 RISR=0x1 TXFLR=0x0 RXFLR=0x0
    wtr: Rx loop timeout after 4000001 iters, rx_len0=6 remaining=5 SR=0x6 RISR=0x1 TXFLR=0x0 RXFLR=0x0 SSIENR=0x1 SER=0x1
    spi-nor spi0.0: probe with driver spi-nor failed with error -110

And, crucially, **boot then continued normally past this point all
the way to login** -- the timeout doing exactly its job (bounding a
failure instead of hanging the whole machine).

Root cause, finally nailed down: exactly **1 of 6** requested bytes
arrives (``remaining`` goes from 6 to 5, then never moves again across
4 million more iterations). This is opcode ``0x9F`` issued via
``dw_spi_exec_mem_op()`` in ``DW_SPI_CTRLR0_TMOD_EPROMREAD`` mode
(hardware auto-continues clocking in ``op->data.nbytes`` frames after
the command, driven by the ``NDF`` field). On this board's ``ls_spi``
instance, that auto-continue mechanism appears to only ever clock one
frame, then stop -- while barebox's own ``c2k_spi.c`` driver
(``do_write_read_transfer`` / ``do_read_only_transfer8``, full-duplex,
no EEPROM-read mode at all) reads many bytes from this exact chip
correctly (the ``crc32 -f /dev/spi0 0+0x100`` test earlier). Combined
with everything separately confirmed working (pinctrl, clock, reset,
CS assertion, the opcode byte itself transmitting) this points at a
genuine limitation/erratum in this SoC's specific "vendor-modified"
instance of the DW APB SSI IP's EEPROM-read auto-continue logic, not
a configuration mistake.

Fix: added a ``no_mem_ops`` flag to ``struct dw_spi`` and checked it
in ``dw_spi_init_mem_ops()`` alongside the existing
``DW_SPI_CAP_CS_OVERRIDE``/``set_cs`` checks that already gate
``exec_op`` registration -- when set, ``spi-nor``'s core naturally
falls back to plain full-duplex (``TMOD_TR``) transfers via the
ordinary ``dw_spi_transfer_one()``/``dw_spi_poll_transfer()`` path
instead of ever calling ``dw_spi_exec_mem_op()``. (Deliberately did
*not* reuse ``DW_SPI_CAP_CS_OVERRIDE`` for this -- that flag makes the
driver write to a real Amazon-Alpine-specific ``DW_SPI_CS_OVERRIDE``
register this controller doesn't have.) ``spi-dw-mmio.c`` reads a new
DT boolean, ``snps,dwc-ssi-broken-eeprom-read``, added to ``&ls_spi``
in ``ls1024a-wdmycloud.dts`` -- board-specific, doesn't affect any
other user of this shared driver. A short 6-byte read like JEDEC ID
fits in a single 8-word FIFO fill regardless of path, so this doesn't
reintroduce the native-CS/shallow-FIFO refill problem for this
particular operation; larger reads (actual flash dumps) going through
the same classic path will need separate verification once this gets
the device probing at all.

Tenth attempt (with mem_ops disabled): the whole transaction now
completes without hanging or timing out at all -- but::

    poll[0]: pre-write tx_len=1 rx_len=1 ...
    poll[0]: post-write ... post-read rx_len=0
    poll[0]: pre-write tx_len=6 rx_len=6 ...
    poll[0]: post-write ... post-read rx_len=0
    spi-nor spi0.0: unrecognized JEDEC id bytes: 00 00 00 00 00 00

Two separate ``->transfer_one()`` calls (opcode, then data), each one
individually clean -- all 6 requested bytes actually get clocked in
(``RXFLR`` reaching the full count each time, ``post-read rx_len=0``)
-- yet the data is all zeros. Root cause: the classic path can
mechanically move the right *number* of bytes, but it cannot keep this
controller's *native* chip select continuously asserted across two
separate ``transfer_one()`` calls -- the Tx FIFO fully drains at the
end of the opcode transfer, which auto-releases CS on this hardware
(the exact "nasty peculiarity" ``dw_spi_poll_transfer()``'s own
comment describes), so by the time the data-phase transfer starts, the
chip sees a fresh, invalid command with no preceding opcode and
returns undefined/zero data. This is the *reverse* problem from the
EEPROM-read one: ``dw_spi_exec_mem_op()`` correctly holds CS the whole
time but its hardware auto-continue is broken; the classic path
correctly clocks every byte but cannot hold CS across multiple
transfers at all.

Fix: keep using ``dw_spi_exec_mem_op()`` (revert disabling mem_ops --
its single atomic, CS-held write-then-read call was the right
mechanism all along), but stop relying on EEPROM-read's hardware
auto-continue for the data phase. Renamed the quirk flag
``no_eeprom_read`` to reflect what it actually now does: when set,
``dw_spi_exec_mem_op()`` selects ``DW_SPI_CTRLR0_TMOD_TR`` (plain
full-duplex) instead of ``TMOD_EPROMREAD`` for Data-IN operations, and
``dw_spi_write_then_read()``'s Rx loop manually pushes dummy ``0x00``
bytes into the Tx FIFO (as much room as available, same technique
``dw_writer()`` already uses in the classic path) to drive the clock
for each byte still wanted, instead of just waiting on Rx FIFO level
-- all while remaining inside the single CS-held call exec_op already
provides. This combines the two previously-separate correct halves:
exec_op's CS continuity, and the classic path's proven-working
manual-clock-drive technique.

Eleventh attempt: real progress -- ``wtr: Rx done`` prints (the Rx
loop completed on its own, no timeout, all 6 bytes drained), but then
immediately::

    RX FIFO overflow detected
    spi-nor spi0.0: probe with driver spi-nor failed with error -5

``dw_spi_check_status()`` (called right after ``write_then_read()``
returns success) finds the ``RXOI`` bit set in ``RISR``. Bug in the
manual-drive loop itself: ``room`` was computed each iteration as
``min(fifo_len - TXFLR, len)`` -- against ``len``, the bytes *still to
be read*, not against how many dummy bytes had *already been pushed*.
Since ``TXFLR`` only reflects what's currently queued and drains on
its own as bytes get clocked out, an already-pushed slot freeing up
made it look like fresh "room" on a later iteration, so the loop could
push more dummy bytes across iterations than ``rx_len`` actually
needed -- clocking in more real data than requested and overflowing
the Rx FIFO once the wanted bytes were drained but extra ones kept
arriving.

Fixed by tracking total dummy bytes pushed in a separate ``tx_pushed``
counter (persisting across iterations, only ever incremented) and
budgeting ``room`` against ``rx_len0 - tx_pushed`` (the true remaining
allowance) in addition to ``len`` and available FIFO space, via
``min3()``.

Twelfth attempt: no more overflow -- ``wtr: Rx done`` completes
cleanly on the first try. But ``spi_nor_detect()`` then falls back to
an SFDP read (opcode ``0x5A``, also completes cleanly through the same
path) since the JEDEC ID it got didn't match anything, and ultimately
reports (still)::

    spi-nor spi0.0: unrecognized JEDEC id bytes: 00 00 00 00 00 00

Checked ``spi_nor_detect()`` in ``drivers/mtd/spi-nor/core.c``: this
final error always dumps the buffer from the *first* read (the
``0x9F`` command) -- the SFDP attempt is just ``spi_nor_match_id()``'s
normal fallback when the ID doesn't match, not a separate bug. So the
first, 6-byte JEDEC ID read is still coming back all zero, mechanics
aside.

Root cause: full duplex means *every* clock edge shifts a byte into
Rx, including the ones spent transmitting the opcode itself -- before
the chip has had any chance to respond. In real EEPROM-read hardware
mode, the command and data phases are separated internally and this
never reaches software; with plain ``TMOD_TR``, it does. That garbage
byte was sitting in the Rx FIFO the whole time (visible in every trace
as ``RXFLR=0x1`` right after "Tx done", *before* the manual-drive Rx
loop ever runs) and was being read as if it were the first real data
byte -- shifting every actual ID byte down by one position and losing
the last one entirely (only ``rx_len`` total bytes get captured).

Fixed by draining and discarding whatever's sitting in the Rx FIFO
immediately after the Tx (command) phase completes and before the
manual-drive Rx loop starts, when ``no_eeprom_read`` is set. Logs how
many bytes it discarded.

Thirteenth attempt confirmed the flush works exactly as sized (logs
say "flushed 1 garbage Rx byte(s)" for the 1-byte opcode, "flushed 5"
for the 5-byte SFDP command+address+dummy phase, matching ``tx_len0``
in both cases precisely) -- but the final JEDEC ID bytes are *still*
all zero. So the off-by-one shift is genuinely fixed, and the deeper
problem is that no real data is arriving during the "read phase" of a
plain ``TMOD_TR`` full-duplex transfer *at all*, garbage or otherwise.

Went back to barebox's own driver for another look, this time at
exactly how it structures a command+data read, rather than just
which chip it reads (``drivers/spi/c2k_spi.c`` in the vendor GPL
source). Its ``switch (op)`` in the transfer loop uses **three**
different ``CTRLR0`` TMOD values depending on the transfer type --
``SPI_TRANSFER_MODE_WRITE_ONLY`` -> TMOD ``0x1`` (transmit-only,
``do_write_only_transfer8()``), ``SPI_TRANSFER_MODE_READ_ONLY`` ->
TMOD ``0x2`` (receive-only, ``do_read_only_transfer8()``), and
``SPI_TRANSFER_MODE_WRITE_READ`` -> TMOD ``0x0`` (TR) only for actual
simultaneous full-duplex transfers. For a command-then-data read like
JEDEC ID, it uses **separate WRITE_ONLY then READ_ONLY sub-transfers**
-- reconfiguring ``CTRLR0`` between them -- not one combined ``TR``
transfer with manual dummy bytes. And critically,
``do_read_only_transfer8()`` writes exactly **one** dummy word
("``/* start the serial clock */``" in its own comment) and then
*only* drains Rx from then on -- it never pushes a second dummy byte.
That single write plus ``TMOD_RO``'s own hardware auto-continue
(driven by the same ``NDF``/``CTRLR1`` mechanism ``TMOD_EPROMREAD``
uses -- confirmed identical in ``dw_spi_update_config()``) is what
generates the rest of the clock cycles, successfully, on this exact
silicon (proven by the working ``crc32`` read).

This gives a concrete, different hypothesis: ``TMOD_RO``'s
auto-continue may work correctly on this controller even though
``TMOD_EPROMREAD``'s doesn't -- they're different enum values, and
the earlier finding only demonstrated the latter is broken.

Restructured accordingly:

- ``dw_spi_exec_mem_op()``'s initial config (covering the opcode/
  address/dummy phase written by ``write_then_read()``'s existing Tx
  loop) now uses ``TMOD_TO`` (transmit-only) instead of ``TMOD_RO``,
  matching barebox's ``WRITE_ONLY`` step -- using ``TMOD_RO`` for the
  *whole* operation would apply "receive-only" even while the opcode
  is being sent, which is presumably why byte 0 was garbage in the
  first place.
- ``dw_spi_write_then_read()`` now takes the ``struct dw_spi_cfg *``
  used for the initial config, and right after the Tx phase completes
  (and the existing command-phase Rx flush -- kept as a safety net,
  though it should now find nothing to flush if ``TMOD_TO`` truly
  doesn't populate Rx during transmission), reconfigures the
  controller to ``TMOD_RO`` with ``ndf = rx_len`` via a second
  ``dw_spi_update_config()`` call, mirroring barebox's own
  reconfigure-between-sub-transfers approach.
- Its Rx loop, when ``no_eeprom_read`` is set, now pushes exactly
  **one** dummy byte on the very first iteration only (matching
  ``do_read_only_transfer8()``'s single "start the serial clock"
  write) instead of one dummy byte per byte wanted, then relies purely
  on ``TMOD_RO``'s hardware auto-continue for the rest.

Fourteenth attempt: confirms ``TMOD_TO`` genuinely doesn't populate Rx
during the opcode phase (``RXFLR=0x0`` at "Tx done" this time, vs
``0x1`` with ``TMOD_TR`` before -- so the command-phase flush now
correctly finds nothing, one theory validated) -- but the data phase
gets *worse*: zero Rx bytes ever arrive at all (not even the one
"garbage" byte seen before), timing out with ``remaining=6`` unchanged
across 4 million iterations, even though the single kick-start dummy
byte visibly transmits (``TXFLR`` back to 0).

Re-read the barebox loop once more, specifically the disable/enable
sequencing: ``op`` (which ``switch`` case runs) is derived once from
``mesg->status``, *outside* the per-transfer loop, meaning the
WRITE_ONLY and READ_ONLY phases barebox uses for a command+data read
must come from **two separate top-level calls** to
``c2k_spi_transfer()`` -- and ``writel(0, SSIENR)`` is the first thing
that function does, every call. So barebox always fully disables the
controller before writing a new ``CTRLR0``/TMOD, never reconfigures it
live. That's also simply the standard, documented DW SSI requirement
(``CTRLR0`` is only safely modified while ``SSIENR`` is 0) -- our
mid-transaction ``dw_spi_update_config()`` call for the ``TMOD_RO``
switch never disabled the chip first, so it likely wasn't taking
proper effect at all.

Added ``dw_spi_enable_chip(dws, 0)`` immediately before, and
``dw_spi_enable_chip(dws, 1)`` immediately after, the ``TMOD_RO``
reconfiguration (``SER`` -- chip-select selection -- lives in a
separate register untouched by ``dw_spi_enable_chip()``, so this
shouldn't affect which device is selected).

Fifteenth attempt: the transfer is now *mechanically flawless* --
``wtr: Rx done`` with no timeout, no overflow, correct byte counts,
``SSIENR``/``SER`` exactly as expected after the switch -- and the
JEDEC ID is *still* all zero. At this point every register-sequencing
theory tried so far (pinctrl, reset, IRQ vs polling, CS continuity,
Rx-shift, FIFO overflow, TMOD_RO vs TMOD_EPROMREAD, disable-before-
reconfigure) has been individually confirmed correct or irrelevant,
yet the chip never returns real data.

Went back to ``commands/update_spi.c`` in the vendor barebox source --
not just *which* chip/registers it uses, but its actual read
implementation, ``read_bytes_page_addr()`` / ``spi_copy_read()``. It
builds *one* combined buffer -- opcode + 3 address bytes + N dummy
zero bytes, e.g. 4+8 = 12 bytes total for its default 8-byte chunk
size -- and calls barebox's own ``spi_write_then_read(spi, cmd_buf,
4+N, b, 4+N)`` with **both lengths equal to the full combined size**.
That in turn reaches ``do_write_read_transfer()``
(``drivers/spi/c2k_spi_common.c``), which is exactly two tight
back-to-back loops -- write all ``*wlen`` bytes, then read all
``*rlen`` bytes -- with **zero logging or delay anywhere in the
hot path**. The caller then discards the first 4 (command-phase
garbage) bytes of the result, keeping only the real trailing data --
structurally the *same* write-all-then-read-all-with-discard shape
this driver's own ``no_eeprom_read`` path already has.

That structural match, plus everything above already being ruled out,
points at a much more mundane explanation: **the extensive
``dev_info()`` tracing added across this whole investigation was
itself introducing the bug.** Every one of those calls sits squarely
inside the CS-held, timing-sensitive part of the transfer, and goes
through the early, synchronous, 115200-baud console -- printk() on
that path genuinely blocks for real, non-trivial time per line, once
per register snapshot, several times per byte-phase. This 8-year-old,
pre-SFDP SPI-NOR part plausibly can't tolerate a multi-millisecond
pause between its command and data phases without resetting its
internal state, silently returning nothing useful for the "corrupted"
transaction rather than erroring out. barebox's own driver, doing the
mechanically equivalent thing with no logging in the hot path at all,
has no such gap.

Removed *all* ``dev_info()``/``dev_err()`` calls from inside
``dw_spi_write_then_read()`` and the register-poking part of
``dw_spi_exec_mem_op()`` -- kept only a single entry-point trace
(before any register writes start) and a single summary trace after
``write_then_read()`` returns (once the transfer, successful or not,
is already fully over). Reverted the ``TMOD_TO``/``TMOD_RO`` phase
split back to plain ``TMOD_TR`` for the whole operation (the extra
disable/reconfigure/enable round trip it required is itself more
opportunity for a gap, and wasn't earning its keep once logging
turned out to be the real problem) -- write-then-read's existing Tx
loop, command-phase Rx flush, and Tx-budget-tracked dummy-byte Rx loop
are structurally the same shape as barebox's proven
write-all-then-read-all-with-discard, just without a printk in the
middle of it this time.

Sixteenth attempt: **real data**, finally --
``unrecognized JEDEC id bytes: 00 ef 30 13 00 00``. The timing theory
was right. ``0xef`` is Winbond's manufacturer ID, and
``SNOR_ID(0xef, 0x30, 0x13)`` is an exact match in upstream
``drivers/mtd/spi-nor/winbond.c`` for ``"w25x40"`` (512 KiB, no SFDP,
no quad -- another old, plain part). Not the ``S25FL064A`` (Spansion,
8 MiB) barebox's own vendor source names this device -- evidently a
real BOM second-source substitution on this specific board/unit that
the vendor firmware's device name was simply never updated for.

The leading ``00`` is a residual one-byte shift -- the command-phase
Rx flush reported ``flushed=0`` in the trace, meaning it found nothing
*yet*: transmitting a byte takes real time (~2us at 4 MHz), and
checking ``RXFLR`` immediately after the Tx loop finishes can
legitimately still read 0 (the hardware hasn't clocked it out yet at
the instant of the check), so the flush loop exited immediately
instead of waiting, and that byte landed as a false byte 0 once the
main Rx loop's own polling caught up to it. Fixed by capturing the
original command length (``cmd_len = dws->tx_len``, taken before the
Tx loop consumes it) and having the flush loop actually *wait* for
that many bytes (bounded, same iteration-limit style as the other
loops here) instead of draining once and moving on.

Updated ``&ls_spi``'s flash node to ``compatible = "winbond,w25x40",
"jedec,spi-nor"`` (mode/frequency unchanged -- already correct, proven
by the working read).

Seventeenth attempt regressed: ``flushed=1`` now correctly matches
``cmd_len`` (the wait-for-the-right-count fix worked, exactly as
intended) -- but the JEDEC ID came back all-zero again, worse than the
shifted-but-real ``ef 30 13`` seen the attempt before. The fix was
locally correct and made things worse anyway, which is a strong signal
the whole *two-phase* design (drain-and-discard, *then* start a fresh
capture) is itself the problem, independent of how carefully each
phase is bounded -- any gap between "last command-phase byte drained"
and "first data-phase dummy byte pushed", however small, is apparently
enough to lose the chip's internal state on this old, timing-sensitive
part.

Went back to barebox's ``read_bytes_page_addr()`` one more time,
looking at structure rather than register values: it builds *one*
combined buffer (command + address + dummy zero bytes) and calls
``spi_write_then_read()`` with a *single* ``n_tx == n_rx`` covering
the whole thing -- there is no separate "discard" phase inside the
transfer at all; the caller only slices the result apart with a plain
``memcpy()`` *after* the hardware transaction is entirely finished.
Rebuilt this driver's ``no_eeprom_read`` path the same way instead of
matching that shape indirectly: added a fixed ``no_eeprom_read_buf[DW_SPI_BUF_SIZE]``
scratch array to ``struct dw_spi`` (sized the same as the existing
``buf`` field, no allocation needed), and restructured the Rx section
of ``dw_spi_write_then_read()`` to capture ``cmd_len + rx_len`` bytes
as one unbroken run into it -- the first ``cmd_len`` of those are the
command-phase bytes already sitting in the Rx FIFO from the opcode
transmission (no separate drain step, no gap: the *same* loop just
keeps reading past them into the real data with no phase boundary in
between) -- then ``memcpy()``'s only the real trailing ``rx_len``
bytes out to the caller's actual buffer once the whole capture loop
has finished. The dummy-byte push budget stays at ``rx_len`` (not
``cmd_len + rx_len``): the command phase's clock cycles already
happened, driven by the opcode bytes themselves; only the data phase
needs manually-driven dummy bytes.

Eighteenth attempt: **success**. JEDEC ID matches ``w25x40``, the chip
probes cleanly (no SFDP fallback attempt even needed -- matched on the
first try), and ``/dev/mtd0``, ``/dev/mtd0ro``, ``/dev/mtdblock0`` all
show up. Confirmed interactively::

    root@MCG1-Devuan:~# ls /dev/mtd
    mtd0       mtd0ro     mtdblock0

Trying to actually dump the flash (``dd if=/dev/mtd0 of=/mnt/111.img
status=progress``, no explicit block size -- ``dd`` defaults to 512
bytes) failed immediately::

    exec_mem_op: enter opcode=0x3 addr.nbytes=3 ... data.nbytes=512
    wtr: done ret=-5 cmd_len=4 ... cap_len=0 ...
    dd: error reading '/dev/mtd0': Input/output error

This is opcode ``0x03`` (plain READ), ``cmd_len`` (opcode + 3 address
bytes) ``= 4``, wanting 512 data bytes -- ``4 + 512 = 516``, over the
scratch buffer's previous 265-byte size (``DW_SPI_BUF_SIZE``, sized
for a bare command -- opcode + address + 256 -- never meant to also
hold an entire data phase). ``cap_len=0`` and the immediate ``-EIO``
is this driver's own new overflow guard correctly refusing to overrun
a buffer that size, not a hardware failure.

Split the scratch buffer's sizing out from ``DW_SPI_BUF_SIZE`` (a
generic constant also used for the plain command-building buffer, not
board-specific to touch) into its own
``DW_SPI_NO_EEPROM_READ_BUF_SIZE`` (4096 + 64 bytes -- comfortably
covers observed 512-byte MTD reads with headroom for a full 4 KiB
block), and resized ``no_eeprom_read_buf`` to match. Also taught
``dw_spi_adjust_mem_op_size()`` -- the ``spi-mem`` core's own hook for
clamping an operation's data length to what the controller can do in
one ``exec_op()`` call -- to additionally clamp against
``DW_SPI_NO_EEPROM_READ_BUF_SIZE - cmd_len`` when ``no_eeprom_read``
is set, so an oversized request gets automatically split into
multiple calls that each fit, instead of erroring out on whichever one
doesn't.

Nineteenth attempt: **fully confirmed**. Dozens of consecutive 512-byte
reads at increasing addresses (``0x0``, ``0x200``, ``0x400``, ... up to
at least ``0x9200``), every single one ``ret=0`` with the correct
``cap_len=516``, no errors, no hangs -- and ``dd`` itself completed
cleanly::

    74+0 records in
    73+0 records out
    37376 bytes (37 kB, 36 KiB) copied, 1.51 s, 24.8 kB/s

This closes out the SPI-NOR boot flash saga: pinctrl, chip-select
timing, the EEPROM-read hardware erratum, the write-then-read
phase-boundary timing sensitivity, and buffer sizing were each real,
independent issues on this specific controller/chip/board combination,
and all are now fixed. ``/dev/mtd0`` reads reliably and repeatedly on
real hardware, not just as a one-shot probe.

PFE (Packet Forwarding Engine) network driver port
====================================================

Staged port of the 3.2.26 vendor ``pfe`` driver
(``kmodules/mspd-c2k/pfe/pfe_ctrl/`` in symops/MCG1-3.2.26), following
the plan recorded when this stage began (see git history for the full
staged plan -- Stage P1 through P10). Progress so far:

Stage P1 (platform_driver skeleton) and P2 (enabled on the board)
    ``drivers/net/ethernet/freescale/pfe/`` brings up the ``apb``/
    ``axi`` (cbus) MMIO windows, the ``pfe``/``pfe_sys`` clocks and
    the ``axi``/``core`` resets, and requests (unarmed) the ``hif``
    IRQ. **Confirmed on real hardware**: ``fsl-ls1024a-pfe
    90500000.pfe: PFE platform skeleton probed`` at 1.1s into boot,
    clean probe, no crash, no regression to the rest of boot (root
    filesystem, userspace, login all unaffected).

    The pre-existing ``ls1024a-wdt: Failed to get watchdog reset
    control`` / ``probe ... failed with error -16`` warning some
    hardware round-trips show near this point in the log is **not**
    caused by this driver -- confirmed present in an earlier boot log
    captured before any PFE work started. Unrelated, already-known,
    non-fatal.

Stage P3 (cbus register layer, hw block bring-up, ddr/iram resources)
    ``pfe_hw_lib.c`` (ported from the vendor tree's shared
    ``pfe/pfe/c2000/pfe.c`` HAL, not pure firmware-side code as first
    scoped -- see commit history) brings up BMU1/BMU2, CLASS, TMU,
    UTIL, and the three EGPI blocks plus HGPI via ``pfe_hw_init()``.

    First real-hardware attempt hit ``error -EBUSY: can't request
    region for resource [mem 0x83000000-0x83001fff]`` / ``Failed to
    map iram resource``: the IRAM window was already owned by the
    pre-existing generic ``mmio-sram`` node (``&iram``, already used
    for ``clk_bypass_bug@fc00``) -- a second ``devm_ioremap_resource()``
    on the same physical range from the pfe node conflicted with it.
    Fixed by dropping the "iram" MMIO claim entirely for now (nothing
    in this driver dereferences it yet); Stage P4's firmware loader
    should go through ``&iram``'s own genalloc pool instead of
    re-claiming the range directly.

    **Confirmed on real hardware** after that fix: all block version
    registers read back real values (CLASS=0x20, TMU=0x1011231,
    BMU1/BMU2=0x21, EGPI1-3/HGPI=0x50, HIF/HIF_NOCPY=0x10, UTIL=0x20 --
    none stuck at 0x0 or 0xffffffff, which would have indicated a dead
    bus), both of ``tmu_init()``'s previously-unbounded polling waits
    (``MEM_INIT_DONE``, ``LLM_INIT_DONE``) completed immediately with
    no timeout, and the full ``PFE platform probed (... ddr=0x03400000
    /12582912 ...)`` message appeared with no regression to the rest
    of boot.

Interesting finding for later stages
    **barebox itself already initializes PFE at boot**, before Linux
    ever runs: every real-hardware boot log shows barebox's own
    ``pfe_hw_init: done`` / ``pfe_firmware_init`` / ``pfe_load_elf``
    (class/tmu/util firmware loaded) lines during its own startup,
    well before the kernel's ``Kernel command line:`` line. Stage P4's
    own firmware loader (below) simply reloads all three PEs from
    scratch on top of whatever barebox left running, the same way the
    vendor driver always did -- **confirmed working on real hardware**,
    no special reset/reuse handling was needed after all.

Stage P4 (firmware loader)
    ``pfe_firmware_init()`` (``pfe_firmware.c``) loads class/tmu/util
    via ``request_firmware()`` against the three ELF blobs built into
    the kernel image (``CONFIG_EXTRA_FIRMWARE`` -- necessary since this
    board has no initramfs and the pfe platform_driver probes at ~1.1s,
    well before the ~2.5-3.4s root mount; see the commit history for
    the full reasoning). **Confirmed on real hardware**: all three
    loads report real section addresses (``class firmware loaded 0xbc0
    0xc3010000``, ``tmu firmware loaded 0x1a0``, ``util firmware loaded
    0x1220`` -- ``0xc3010000`` exactly matches ``PE_LMEM_BASE_ADDR``,
    a good sanity check that the ELF section classification logic is
    correct), the ``.version`` ELF section parses correctly
    (``PFE binary version: pfe_nas_2_00_3``), and the full
    ``PFE platform probed`` message appears with no regression to the
    rest of boot.

    Also confirmed on real hardware: this board's ``system_rev`` really
    is ``0``, which per the vendor driver's own logic would select the
    ``util_c2000_revA0.elf`` firmware variant -- a file this GPL source
    drop doesn't actually contain (only the non-revA0 ``util_c2000.elf``
    is present). The driver logs a warning and loads ``util_c2000.elf``
    anyway; it loaded and enabled without error, but whether it behaves
    correctly for revA0 silicon specifically is unverified and won't
    surface until UTIL's actual runtime behavior is exercised in a
    later stage. If util-related packet processing misbehaves once
    traffic flows, this substitution is the first thing to revisit.

Stage P5 (HIF DMA descriptor ring + ISR)
    ``pfe_hif.c``/``pfe_hif_lib.c`` bring up the HIF Rx/Tx descriptor
    rings, the shared-memory Rx buffer pool, and the real ISR/NAPI Rx
    path -- the highest-risk file in this whole port going in (a fully
    custom host<->firmware DMA protocol, no mainline equivalent). Also
    fixed a latent ordering bug from Stage P4: probe() now matches the
    vendor's hw_init -> hif_lib_init -> hif_init -> firmware_init
    sequence, since firmware_init's last step enables the PE cores and
    they can only usefully drive HIF traffic once HIF is ready to
    receive it (Stage P4 had firmware_init running immediately after
    hw_init, before HIF existed at all).

    **Confirmed on real hardware, first attempt** -- no crash, no hang,
    despite this being the stage the plan flagged as most likely to
    need retries: ``pfe_hif_lib_init: pkt size 1544, rx buffers 256``
    (the shared buffer pool, real values matching PFE_PKT_SIZE and
    HIF_RX_DESC_NT), the full ring/NAPI/IRQ setup inside
    ``pfe_hif_init()`` completes, and ``PFE platform probed`` appears
    with no regression to the rest of boot (reached login normally).
    Not yet exercised: no client is registered (Stage P7-P9), so no
    real Rx/Tx traffic has actually flowed through the ring yet -- this
    confirms the ring initializes cleanly, not that packets survive it
    end to end.

Cosmetic, rootfs-side, not a kernel issue
    The Devuan rootfs's init scripts still try ``modprobe pfe`` (the
    old vendor kernel's module name) and fail with ``Module pfe not
    found`` since this port's driver is built in (``=y``), not a
    loadable module. Harmless -- boot continues -- and out of scope
    for this kernel repo to fix (rootfs-side init script).

Stage P6 (``pfe_ctrl.c`` -- control-message channel to the PE firmware)
    ``pfe_ctrl.c``/``pfe_ctrl.h`` add the mailbox protocol used to talk
    to running CLASS/TMU/UTIL PE firmware: ``pe_sync_stop()``/
    ``pe_start()`` (pause/resume packet processing on a set of PEs) and
    ``pe_request()``/``tmu_pe_request()`` (ask a PE to copy data
    to/from its own DMEM from/to DDR, used by later stages to push
    per-GEM config down to the firmware). Deliberately dropped: the
    vendor pfe_ctrl.c's "FCI" control plane
    (``__pfe_ctrl_cmd_handler()`` in ``pfe/pfe/c2000/__pfe_ctrl.c``,
    dispatching into a dozen ``module_*.c`` files implementing IP
    routing/bridging/VLAN/PPPoE/IPsec/tunnel/WiFi-offload/RTP-relay/
    multicast/QoS) -- none of it is needed to pass plain Ethernet
    traffic through GEM0-2 (Stage P7-P9), matching the same scope cuts
    already made for ``pfe_vwd.c``/``pfe_pci.c``/``pfe_diags.c``/
    ``pfe_sysfs.c``/``pfe_mspsync.c``/``pfe_unit_test.c`` earlier.
    Also dropped along with it: the periodic ``ctrl->timer_thread``,
    the ``dma_pool``/``dma_pool_512``/``null_ct`` conntrack-entry
    allocators, and the route-table/IPsec-LMEM address fields -- all of
    it existed only to support the FCI apparatus.

    A second, independent scope cut: the vendor driver finds each PE's
    mailbox address by building host-side "shadow" copies of the
    mailbox structs into specially-named linker sections inside its own
    ``pfe_ctrl.ko`` (the ``CLASS_DMEM_SH2()``-family macros in
    ``pfe_ctrl_hal.h``), then computing the PE-side address from the
    relative offset between that shadow section and this driver's own
    copy of it. That trick needs the host driver to be a loadable
    module with its own linker script fragment -- doesn't fit a
    built-in (``=y``) driver. Since the firmware ELFs are unstripped,
    this port instead looks the exact ``sync_mbox``/``msg_mbox``
    symbols up directly in each firmware's ``.symtab``
    (``pfe_firmware.c:get_elf_symbol_addr()``) and uses their
    ``st_value`` as-is -- confirmed via ``arm-linux-gnueabihf-readelf
    -sW`` that those symbols exist, with sizes matching
    ``struct pe_sync_mailbox``/``struct pe_msg_mailbox``, in all three
    firmware blobs in this tree (``class_c2000.elf``: ``msg_mbox`` @
    ``0x1600``, ``sync_mbox`` @ ``0x1860``; ``tmu_c2000.elf``:
    ``msg_mbox`` @ ``0x4c8``, ``sync_mbox`` @ ``0x5a8``;
    ``util_c2000.elf``: ``msg_mbox`` @ ``0x1288``, ``sync_mbox`` @
    ``0x1d18``). ``pfe_firmware_init()`` populates
    ``pfe->ctrl.sync_mailbox_baseaddr[]``/``msg_mailbox_baseaddr[]``
    for the matching PE id range right after each firmware image loads;
    ``pfe_ctrl_init()`` itself is now just a ``mutex_init()``, run after
    ``pfe_firmware_init()`` (matching vendor ordering) since those
    addresses aren't known until the ELF symbol tables have been
    parsed.

    **Build-only verification** -- per the porting plan, this stage has
    no independently observable boot-time behaviour of its own (nothing
    calls ``pe_request()``/``pe_sync_stop()``/``pe_start()`` yet; that
    starts in Stage P7-P9 once a ``net_device`` exists to drive them),
    so it's checked by a clean build rather than its own hardware round
    trip -- treated as one combined checkpoint with Stage P7.

Stage P7 (``pfe_eth.c`` -- net_device/MDIO/PHY for GEM0)
    New files ``pfe_eth.c``/``pfe_eth.h`` add a ``net_device`` for GEM0,
    the GEMAC hardware bring-up it needs (a new GEMAC block in
    ``pfe_hw_lib.c``, ported from the vendor's shared ``pfe.c`` HAL same
    as the BMU/GPI/CLASS/TMU/UTIL blocks in Stage P3), an MDIO bus, and
    PHY connect/link handling. Also added: ``hif_lib_client_register()``/
    ``hif_lib_client_unregister()`` in ``pfe_hif_lib.c`` -- the missing
    other half of the client registration Stage P5 already built the
    HIF-driver side of (``pfe_hif.c``'s own ``pfe_hif_client_register()``
    was already complete from P5; only the *client library* side that a
    real client calls was missing).

    Ported from the vendor's ``pfe_ctrl/pfe_eth.c`` (2846 lines) -- almost
    none of that file's size carried over; see ``pfe_eth.h``'s banner
    comment for the itemized list of what's structurally out of scope
    (ethtool/sysfs stats, VLAN offload, and everything already dropped in
    earlier stages). Two things worth calling out specifically:

    - **No per-GEM NAPI.** The vendor driver gives each net_device its
      own lro/low/high NAPI triple. This port centralized Rx polling at
      the HIF level back in Stage P5 (a single shared
      ``struct pfe_hif.napi``, since there's only one physical Rx DMA
      ring shared by every client, demultiplexed by client id in the
      descriptor header) -- a cleaner match for the actual hardware than
      the vendor's per-client design, and it means Stage P7 doesn't need
      a second NAPI layer at all. ``hif_lib_client_register()`` is what
      makes GEM0 reachable from the existing Rx path; actually consuming
      what lands in its client Rx queue into real skbs is Stage P8's job.
    - **``.ndo_start_xmit`` is a stub that drops.** It has to exist --
      ``dev_hard_start_xmit()`` calls it unconditionally once a netdev is
      ``IFF_UP`` (e.g. for an outgoing ARP the instant link comes up) --
      but the real HIF Tx submission path is Stage P8, not P7.

    **PHY address unconfirmed for this board (plan's own "open risk
    #1").** No mainline driver or board file exists for this exact unit.
    The only real data point found anywhere in the GPL source drop is in
    ``arch/arm/mach-comcerto/board-c2kevm.c``, which has been hand-edited
    with initials-attributed comments crossing out the EVM template's
    ``phy_id = 4`` in favor of ``phy_id = 0``, identifying a Broadcom PHY,
    and widening the MDIO probe mask to addresses 0-7 -- circumstantial
    (can't prove it's this exact board's own edit vs. some other
    Comcerto-derived product sharing the same GPL drop), but the only
    real evidence available. Rather than trust a decade-old comment, the
    DTS deliberately ships with **no ``phy-handle``** on the ``gem0``
    node yet: ``pfe_eth.c`` registers the MDIO bus with a full address
    scan (``phy_mask = 0``, addresses 0-31) and logs every response via
    ``mdiobus_read()`` of ``MII_PHYSID1``/``MII_PHYSID2`` directly, so the
    real address gets confirmed empirically from a hardware boot log --
    the same way the SPI-NOR chip ID was settled earlier in this project
    -- rather than guessed. Once confirmed, adding an ``ethernet-phy@N``
    child node and a ``phy-handle`` on ``gem0`` needs no driver code
    change. Without a PHY connected, link simply stays down for this
    first round trip -- expected, not a failure.

    The MDC clock divisor (96) is a second, much lower-stakes data point
    from that same board file comment block -- any of the GEMAC's valid
    divisor settings keeps MDC well under a PHY's maximum clock spec at
    this SoC's clock rates, so getting it exactly right isn't safety-
    critical the way the PHY address is.

    ``select PHYLIB`` was added to this driver's Kconfig entry
    (``CONFIG_MDIO_BUS``/``CONFIG_OF_MDIO`` follow automatically, no new
    ``ls1024a_defconfig`` lines needed for those); a stray
    ``CONFIG_WIRELESS``/``CONFIG_PTP_1588_CLOCK`` reordering that showed
    up in a ``savedefconfig`` pass while checking this was confirmed to
    reproduce identically with the ``select PHYLIB`` line removed
    entirely -- pre-existing scratch-tree-vs-defconfig drift unrelated
    to this stage (the same class of issue as Stage P1's defconfig
    bloat bug), left untouched rather than folded into this change.

    **Confirmed on real hardware** -- the plan's round #4, specifically
    about observing the MDIO scan results rather than confirmed traffic
    (that's Stage P8's DHCP+ping round). Full boot to login, no crash,
    no regression to any earlier stage::

        fsl-ls1024a-pfe 90500000.pfe: mdio: PHY responding at address 0 (id 0362:5e6a)
        fsl-ls1024a-pfe 90500000.pfe: gem0 registered as eth0 (phy-mode rgmii-id)
        fsl-ls1024a-pfe 90500000.pfe: PFE platform probed (apb=(ptrval) cbus=(ptrval) ddr=0x03400000/12582912 hif_irq=27)
        ...
        fsl-ls1024a-pfe 90500000.pfe eth0: no phy-handle in DT yet -- link will stay down

    Exactly one PHY responded, at address 0 -- matching the
    ``board-c2kevm.c`` comment's crossed-out ``phy_id=4`` -> ``phy_id=0``
    identified above, now with real evidence instead of a decade-old
    comment. The last line confirms the deliberate no-``phy-handle``
    fallback path worked exactly as designed: ``pfe_eth_open()`` (run
    from the rootfs's own ``ifup`` during boot) logged and skipped
    ``pfe_phy_init()`` rather than guessing an address or failing.

    Followed up in the same round (no code change needed, exactly as
    planned above): added ``ethernet-phy@0``/``phy-handle`` to the DTS
    now that address 0 is confirmed, and ``CONFIG_BROADCOM_PHY=y`` to
    ``ls1024a_defconfig`` given the board file's own identification of
    a Broadcom part -- if the ID doesn't match that driver's table,
    Generic PHY remains the fallback, so this can't regress link-up
    either way.

    **PHY connect confirmed on the very next boot** (``putty.log.37``,
    same session): with the ``phy-handle`` now wired, ``of_phy_get_and_
    connect()`` succeeded and the guessed ``CONFIG_BROADCOM_PHY=y`` paid
    off -- the ID (``0362:5e6a``) resolves to a real, named part::

        [   10.220000] Broadcom BCM54612E ls1024a-pfe-mdio:00: attached PHY driver (mii_bus:phy_addr=ls1024a-pfe-mdio:00, irq=POLL)

    No crash, no regression, clean boot to login as before. Not yet
    seen: an actual ``phy_print_status()`` "Link is Up" line --
    ``pfe_eth_adjust_link()`` only prints on a state *transition*, and
    the initial callback with the link still down doesn't count as one
    (see the function's own logic), so this is consistent with no
    Ethernet cable being plugged into this GEM0 port on the test bench
    for this particular boot, not a driver problem. Worth a cabled
    round trip before or alongside Stage P8 to see the transition print
    and confirmed autoneg speed/duplex, but doesn't block starting P8.

Stage P8 (Tx/Rx traffic path for GEM0)
    ``pfe_eth_send_packet()`` (``.ndo_start_xmit``) and
    ``pfe_eth_rx_drain()``/``pfe_eth_event_handler()`` replace the Stage
    P7 Tx stub and give GEM0 an actual data path, on top of new client-
    library plumbing in ``pfe_hif_lib.c``: ``hif_lib_xmit_pkt()`` (queues
    one full, non-fragmented packet -- the vendor's TSO/fragmented-skb
    submission path via ``__hif_lib_xmit_pkt()`` isn't ported),
    ``hif_lib_tx_get_next_complete()`` (dequeues a completed packet's skb
    so it can be freed -- also where the underlying HIF Tx descriptor's
    DMA mapping gets torn down, via ``hif_tx_done_process()``, already in
    place since Stage P5), ``hif_lib_receive_pkt()`` (dequeues one
    received packet's buffer), and ``hif_lib_event_handler_start()`` (the
    Rx re-arm half of ``hif_lib_indicate_client()``'s edge-triggered
    wakeup -- without it, a client's Rx queue would only ever be
    indicated once, the very first time a packet arrives, and never
    again).

    No stop/wake-queue Tx flow control, no TSO, no multi-queue QoS
    classification (everything submits on a single fixed queue,
    ``PFE_ETH_TXQ``) -- none of it is needed to reach this stage's own
    bar (DHCP+ping), and the 1024-deep Tx ring (``EMAC_TXQ_DEPTH``) is
    comfortably more than that traffic pattern needs; a full ring just
    drops the packet, the same way any driver without stop/wake-queue
    wiring should rather than returning ``NETDEV_TX_BUSY`` with nothing
    to ever un-stick it.

    **Rx consumption has no second NAPI layer**, unlike the vendor's
    per-GEM lro/low/high NAPI triple. Stage P5 already centralized Rx
    polling at the HIF level (a single physical Rx DMA ring shared by
    every client, demultiplexed by client id) -- ``pfe_eth_rx_drain()``
    runs directly from ``pfe_eth_event_handler()``, itself called
    synchronously from ``hif_lib_indicate_client()`` while already inside
    that shared HIF-level NAPI poll's call stack, so there's nothing to
    separately schedule. Rx buffers are wrapped into skbs with
    ``build_skb()`` (zero-copy) rather than a copy -- ``PFE_BUF_SIZE``
    (2048) was already sized for exactly this back in Stage P5, mirroring
    the vendor driver's own non-mainline ``alloc_skb_header()`` kernel
    addition with the stock ``build_skb()`` equivalent.

    Multi-descriptor (jumbo/LRO) Rx reassembly isn't ported --
    ``PFE_PKT_SIZE`` (1544) covers any standard MTU + headers in a single
    HIF descriptor, so a packet split across more than one should never
    actually occur; ``pfe_eth_rx_drain()`` drops defensively (with a
    rate-limited warning) rather than mishandle it if it ever does.

    **Two real-hardware bugs found and fixed during the round #5
    round-trip** (both invisible in a build-only check -- neither has
    any effect until MDIO keeps working past early boot, or until a
    real packet actually reaches ``.ndo_start_xmit``):

    - **Missing per-GEM "extphy" clock breaks MDIO a couple seconds
      after boot.** Each GEM has its own reference clock line
      (``LS1024A_CLK_EXTPHY0/1/2`` in ``clk-ls1024a.c``), separate from
      the ``pfe`` node's shared ``"gemtx"`` -- this port never requested
      it. Symptom, confirmed via a temporary raw-register debug build:
      MDIO reads kept completing (the bus protocol handshake itself
      doesn't need this clock) but silently returned the *previous*
      successful read's value for any register/PHY address, forever,
      starting right around when the kernel's own "``clk: Disabling
      unused clocks``" late-boot step runs -- it physically gated this
      line off, since nothing had ever asked for it. Traced with a
      from-scratch, timestamped, independent MDIO poll (a kernel timer
      unrelated to this driver's own open()/close() call sequence) to
      pin the exact moment against every other boot-time log line,
      after two earlier hypotheses (a PHY-mode-select register write
      disrupting the MDIO block; the already-known ``"gemtx"`` clock
      being gated) were each build-tested and real-hardware-tested and
      ruled out in turn. Fixed by requesting and enabling ``"extphy"``
      in ``pfe_eth_probe_gem()`` (``of_clk_get_by_name()``, since a GEM
      child node isn't its own ``struct device`` -- cleanup wired to the
      parent ``pfe`` device's lifetime via
      ``devm_add_action_or_reset()``).
    - **``pfe_eth_send_packet()`` self-deadlocked on the very first real
      packet.** It wrapped its call to ``hif_lib_xmit_pkt()`` in
      ``hif_tx_lock()``/``hif_tx_unlock()``, but ``hif_lib_xmit_pkt()``
      already calls ``hif_xmit_pkt()`` (Stage P5, ``pfe_hif.c``), which
      takes the *same* ``hif->tx_lock`` itself (and already calls
      ``hif_tx_dma_start()`` on success, making the outer call's own
      ``hif_tx_dma_start()`` redundant too). ``spin_lock_bh()`` isn't
      reentrant, so the second acquire spun forever -- surfaced as an
      ``rcu: INFO: rcu_sched self-detected stall on CPU`` with CPU 1
      stuck at ``_raw_spin_lock_bh`` inside ``hif_xmit_pkt()``, called
      from a ``kworker`` thread's ``mld_ifc_work`` (the kernel's own
      IPv6 multicast-listener report going out over eth0 -- not
      DHCP/ping traffic, but real enough to hit the same TX path).
      Fixed by removing the redundant outer lock and DMA-start call.

    Confirmed on real hardware after both fixes: sustained MDIO health
    past the "disabling unused clocks" mark (independently verified for
    a full minute via the same debug timer used to find the clock bug),
    and TX submission surviving real IPv6 multicast-report traffic
    without deadlocking.

    **Link-up itself confirmed on the very next boot** (no cable/switch
    change from earlier attempts -- only these two fixes)::

        [    9.890000] fsl-ls1024a-pfe 90500000.pfe eth0: Link is Up - 1Gbps/Full - flow control off

    ``ethtool eth0`` shows real, populated link-partner autonegotiation
    data (not just this side's own advertised modes) -- ``Link partner
    advertised link modes: 10baseT/Half 10baseT/Full 100baseT/Half
    100baseT/Full 1000baseT/Full``, ``Speed: 1000Mb/s``, ``Duplex:
    Full``, ``Link detected: yes`` -- and ``ifconfig`` shows the
    ``RUNNING`` flag (carrier present) with ``TX packets 10``, zero
    errors, zero drops. No RCU stall, no crash, clean boot to login as
    before.

    **Rx investigation: five real bugs found and fixed before a single
    packet reached the host.** Link-up (above) only proved TX and the
    PHY/MDIO path; Rx stayed at 0 packets for many further real-hardware
    rounds after that, with ``hif_isr()`` never firing at all (confirmed
    via an unconditional entry ``pr_info()`` and ``/proc/interrupts``)
    even under sustained real broadcast/DHCP/ARP traffic on the wire.
    Extensive register/IRQ-level comparison against a reference 3.2.26
    vendor kernel/``pfe.ko`` built and run on the exact same hardware
    (needed real logging added to that tree too, since its own source
    gave no better a starting point) eventually turned up five
    independent, real bugs -- none alone sufficient, each confirmed by
    its own real-hardware round:

    - **DDR carve-out mapped write-back cacheable** (``devm_memremap(...,
      MEMREMAP_WB)``) instead of uncached, unlike the vendor driver's own
      ``ioremap()`` of the same resource -- PFE's AXI bus master has no
      visibility into the ARM cores' cache, so CPU writes into the route
      table/BMU2 pool/firmware DDR sections could sit in cache
      indefinitely, invisible to PFE. Fixed with ``devm_ioremap()``.
    - **No real reset pulse before reconfiguring PFE.** This board's
      barebox fully brings up PFE (loads and enables CLASS/TMU/UTIL
      firmware) before Linux boots at all; ``pfe_platform_probe()`` only
      ever called ``reset_control_deassert()``, never ``assert()`` first
      -- against already-deasserted lines that's a no-op, so firmware
      reconfiguration/reload happened out from under PE cores that were
      never actually stopped. Fixed with a genuine assert-then-deassert
      pulse.
    - **``CHIP_REVISION()`` always read 0** (the generic ARM kernel's
      never-populated ``system_rev``), silently triggering a
      rev-0-only vendor workaround (``control_qm.c``'s "LOG: 68855",
      forcing every TMU queue depth down to 31) on hardware confirmed
      (by reading the real SoC chip-ID register, as the vendor driver
      does) to actually be rev 1. Fixed by reading that register
      directly into a new ``pfe_chip_rev`` global.
    - **CLASS PE firmware never learns a GEM's MAC address or interface
      index.** The vendor host driver writes both into the firmware's
      ``phy_port[]`` DMEM array as a side effect of an FCI command this
      port never implements. Without it, TX worked but RX stayed at 0
      packets forever regardless of any of the other fixes -- this was
      the one that actually got a packet delivered to the host for the
      first time. Fixed by writing it directly in
      ``pfe_eth_class_register_port()``, called from ``.ndo_open``.
    - **Random MAC address.** This board's barebox has no per-unit DT, so
      the real MAC reaches Linux via a ``mac_addr=`` kernel command line
      parameter (the same mechanism the 3.2.26 vendor kernel's ATAG boot
      path used) -- never parsed here, so every GEM fell back to
      ``eth_hw_addr_random()``, which the ``phy_port[]`` registration
      above would then never match real inbound traffic against. Fixed
      with a ``__setup("mac_addr=", ...)`` handler mirroring the
      vendor's own ``mac_addr_setup()``.

    Once those five landed, real traffic started arriving -- but Rx
    stalled again after ~60 real packets (DHCP + a few pings), followed
    minutes later by an unrelated-looking kernel panic from
    ``ksoftirqd`` spinning at 100% CPU. **A sixth bug**: Rx buffer
    refill called ``dma_alloc_coherent(..., GFP_ATOMIC)`` on every single
    received packet, which -- unable to sleep -- is forced through the
    kernel's small, fixed-size boot-time atomic DMA pool rather than the
    much larger general-purpose backing available to ordinary
    ``GFP_KERNEL`` allocations; a modest traffic burst exhausts it, the
    descriptor it would have refilled never gets re-armed, and NAPI
    (which treats "client queue full" as "budget exhausted, poll again
    immediately") busy-spins forever on a descriptor that will never
    move again. Fixed by pre-allocating a pool of spare buffers once via
    ``GFP_KERNEL`` and recycling them via plain array push/pop
    (``pfe_hif_spare_buf_get()``/``_put()``, ``pfe_hif_lib.c``) -- no
    allocator call, atomic or otherwise, on the Rx hot path at all.

    **Confirmed on real hardware, DHCP lease and sustained ping (the
    plan's literal round #5 bar) both closed out**: ``ping 8.8.8.8`` at
    0% packet loss, and a further round moving 280+ MiB of real traffic
    with zero Rx errors/drops -- versus permanent Rx stall and an
    eventual kernel panic before the sixth fix above.

Stage P9 (GEM1/GEM2 -- confirmed not applicable to this board)
================================================================

The plan called for porting GEM1/GEM2 by the same pattern as GEM0 once
it worked. Before starting, checked with the physical hardware first
(the same discipline that caught the SPI-NOR chip misidentification and
the PHY address 4 vs. 0 mismatch earlier in this port): **this board has
exactly one physical RJ45 Ethernet jack**, user-confirmed. The vendor
tree's production ASIC board file (``board-c2kasic.c``, not just the EVM
one) does configure all three GEMs (``phy_id`` 4/5/6 for GEM0/1/2
respectively) -- PFE's three-EMAC hardware is shared silicon across
WD's whole product line, and that board file is a shared base other
SKUs (routers, multi-port NAS models) build on, not a description of
this specific board's wiring. GEM0's own real PHY address (0, not the
board file's assumed 4) already proved this board deviates from that
shared default once; GEM1/GEM2 having no PHY populated at all is the
same kind of deviation, not a contradiction of it.

**No DT nodes added for GEM1/GEM2** -- there's no PHY to describe a
``phy-handle`` for, so a node would just be dead configuration with
nothing to confirm against real hardware, the same reasoning already
applied to RTC (no backup battery on this board) and PCIe (physically
unused on this SKU). Treated as not applicable, not pending work.

Two-LED Stage P8 finding, closed out here rather than left as P9 work:
only one of this port's two RJ45 LEDs ("Link") lit under this port, vs.
both under the 3.2.26 vendor kernel, confirmed purely PHY-register-
driven (real cable unplug/replug on this port didn't light it either,
ruling out any link-event-timing explanation). Root cause found by a
full blind sweep of every BCM54xx shadow (0x00-0x1f) and expansion
register this driver's code selects, diffed byte-for-byte against the
same sweep run on the vendor kernel: shadow register 0x0b (RGMII Mode
Selector) read its untouched hardware default here vs. 0x08c on the
vendor kernel -- exactly the value the vendor's WD-specific
``bcm54610_config_init()`` hack writes there as an undocumented side
effect of what its own comment calls "disable half-duplex" (the actual
half-duplex-disable part is two separate MII register writes; the
shadow-0x0b write is a third, distinct action the same function
performs). This port never carried that write over -- inspected and
dismissed early in the investigation as "not LED-related" from the
register's generic name alone, without checking the value the vendor
was actually writing or what dropping it might affect. Restoring it
(``pfe_phy_restore_led_mode()``, ``pfe_eth.c``) fixed it; confirmed on
real hardware, both LEDs now show real link/activity status matching
the vendor kernel.

Watchdog reset-control conflict with syscon
============================================

Every real-hardware boot log since the very first Stage 2 capture
showed this, unrelated to any of the PFE work above -- root-caused and
fixed while working through this doc:

::

    WARNING: CPU: ... at drivers/reset/core.c:799 __reset_control_get_internal+0x108/0x198
    ...
    __reset_control_get_internal from __of_reset_control_get+0x168/0x1cc
    __of_reset_control_get from __devm_reset_control_get+0x68/0xf0
    __devm_reset_control_get from ls1024a_wdt_probe+0xa0/0x2e8
    ...
    ls1024a-wdt 90450000.timer:watchdog: Failed to get watchdog reset control
    ls1024a-wdt 90450000.timer:watchdog: probe with driver ls1024a-wdt failed with error -16

Root cause: ``ls1024a_wdt_probe()``'s own
``syscon_node_to_regmap(dev->of_node->parent)`` call (needed to reach
the "timer" node's registers, and the line immediately before the
failing one) triggers, as a side effect, the generic syscon
framework's registration of that same parent node
(``drivers/mfd/syscon.c``, ``of_syscon_register()`` with
``check_res=true``): it does its own
``of_reset_control_get_optional_exclusive()`` +
``reset_control_deassert()`` on the node's reset line and, on the
success path, never calls ``reset_control_put()`` -- that handle sits
in the reset framework's list forever, held exclusively. The
watchdog driver's very next lines then ask for the *same* reset line
again (``devm_reset_control_get_shared()``, same parent node), which
always collides with the permanently-held exclusive syscon handle --
shared or not doesn't matter, ``drivers/reset/core.c``'s
``__reset_control_get_internal()`` only allows a second handle on an
already-held line when both sides are shared, or via a narrow
unshared-and-unacquired carve-out a shared request doesn't qualify
for.

Fix: since syscon's own registration already deasserts this reset
before ``ls1024a_wdt_probe()`` reaches the ``syscon_node_to_regmap()``
call, there was nothing left for the watchdog driver to legitimately
request or deassert -- removed entirely from
``drivers/watchdog/ls1024a_wdt.c`` (the ``struct reset_control *``
field, the get+deassert in probe, the assert in remove). The DT
``resets``/``reset-names`` property stays on the "timer" node itself
(still genuinely needed, by syscon's own registration).

**Confirmed on real hardware**: the warning and the two error lines
are completely gone from the boot log, with no regression to the rest
of boot.

Ping RTT quantized to 4ms -- missing hardware clocksource
===========================================================

Noticed on real hardware: every ``ping`` RTT, on any interface
(IPv4, IPv6, even loopback-adjacent LAN hosts), landed on an exact
multiple of 4ms -- e.g. ``9.97 ms``, ``14.4 ms``, never anything like
``11.2 ms``.

Root cause: this board's only registered clocksource was the generic
``jiffies`` fallback (``rating`` 1), whose resolution is exactly one
tick -- ``1/CONFIG_HZ``, which is ``4ms`` at this board's
``CONFIG_HZ=250``. Every ``CLOCK_MONOTONIC`` timestamp, and therefore
every RTT userspace ``ping`` computes from two such timestamps, is
quantized to that same step. Confirmed early in boot::

    [    0.000000] clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: 7645041785100000 ns
    [    0.000000] sched_clock: 32 bits at 250 Hz, resolution 4000000ns, wraps every 8589934590000000ns

The obvious candidate to fix this, the ARM Cortex-A9 Global Timer
(``global_timer@fff00200`` in ``ls1024a.dtsi``), is left
``status = "disabled"`` -- and for good reason not to blindly enable
it: its clock (``LS1024A_CLK_A9DP_MPU``) is a fixed ``/4`` divider of
the CPU's own core clock (``LS1024A_CLK_A9DP``), registered via
``clk_hw_register_fixed_factor(NULL, "a9dp_mpu", "a9dp", 0, 1, 4)`` in
``drivers/clk/clk-ls1024a.c`` -- sharing a clock domain with the CPU
core is exactly the kind of thing that has bitten this port before.

The 3.2.26 vendor tree never used the Global Timer either. Its own
``arch/arm/mach-comcerto/time.c`` drove both ``sched_clock()`` and its
clocksource from a completely different, independent peripheral: a
free-running 32-bit up-counter (vendor name "timer2") inside the SoC's
own "timer" register block at ``0x90450000`` -- the *same* block, and
the same syscon regmap, already used by
``drivers/watchdog/ls1024a_wdt.c`` in this port, clocked directly off
``axi`` (200 MHz on this board, entirely independent of the CPU core
clock).

Fix: ``drivers/clocksource/timer-ls1024a.c``, a new driver that
configures this same counter (low bound 0, high bound the full 32-bit
span, ctrl 0 -- the vendor's own proven-working configuration for it,
replicated verbatim since the ctrl bit's meaning is otherwise
undocumented) and registers it as a clocksource (``rating`` 250, well
above jiffies' 1). DT: a new ``clocksource`` child node next to
``watchdog`` under the existing ``timer@90450000`` node in
``ls1024a.dtsi``.

**Confirmed on real hardware**::

    [    2.336000] clocksource: ls1024a-timer2: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: 9556302233 ns
    [    2.340000] clocksource: Switched to clocksource ls1024a-timer2
    [    2.344000] ls1024a-clksrc 90450000.timer:clocksource: LS1024A hardware clocksource running at 200000000 Hz

``ping`` RTTs afterward show real sub-millisecond variation (e.g.
``9.97``, ``14.4``, ``9.79``, ``9.59 ms`` in one IPv4 run) instead of
the previous 4ms staircase.

Toolchain note
==============

Unlike the 3.2.26 port (which needed a matched-era GCC 4.7 + binutils
2.22 toolchain to build at all), v6.18.50 builds cleanly with the
modern host toolchain (GCC 13, binutils 2.42) already present in this
environment -- no compiler-version workarounds were needed anywhere
in this port.
