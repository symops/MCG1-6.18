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

    No LS1024A PWM driver exists yet in the Bonstra fork, and the RGB
    combining logic doesn't map cleanly onto the generic
    ``gpio-leds`` binding, so this was deliberately
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
        -n "Linux-6.18.46-ls1024a-wdmycloud" \
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
(``mcg1-devuan.log``), now running unmodified under v6.18.46.

Remaining issues visible in this boot, all expected and separate from
the kernel-boot work above:

- ``modprobe: FATAL: Module pfe not found`` / ``Cannot find device
  "eth0"`` / DHCP failure -- the PFE network driver doesn't exist yet
  (see "Known gaps" above; this is the big Stage 3 item).
- ``/etc/init.d/wd-leds: line 16: ... No such file or directory`` --
  no LS1024A PWM/LED driver yet (also already listed above).
- ``rsyslog: Permission denied`` and ``ntpsec: Permission denied``
  during service startup -- not yet root-caused; possibly an
  LSM/capability default that differs from what this rootfs's init
  scripts expect from the old 3.2.26 kernel. Follow-up.
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

Toolchain note
==============

Unlike the 3.2.26 port (which needed a matched-era GCC 4.7 + binutils
2.22 toolchain to build at all), v6.18.46 builds cleanly with the
modern host toolchain (GCC 13, binutils 2.42) already present in this
environment -- no compiler-version workarounds were needed anywhere
in this port.
