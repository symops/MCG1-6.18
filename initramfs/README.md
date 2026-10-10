# Rescue initramfs (`uRamdisk`)

This directory is the source tree for an optional rescue/test initramfs,
analogous to the one `symops/pelican-6.18` (and its base, `symops/monarch-6.18`)
ships for the RTD1295/1296 WD boards, adapted to this board's own facts.
It is **not** part of the board's normal boot path -- the shipped
Devuan/Debian rootfs boots directly via barebox with no initrd. This
image is wired to partition 6 of the SATA disk as an alternate,
optional path for testing/recovery (see `BUILDING.md` section 9),
without touching the normal boot setup.

## Contents

- `bin/busybox` -- statically linked busybox (1.36.1), cross-compiled
  with `arm-linux-gnueabihf-gcc`, `CONFIG_STATIC=y`, built from an
  `allnoconfig` base with only the applets this `init` script actually
  uses enabled: ash/sh, mount, umount, switch_root, mkdir, mknod, ls,
  cat, echo, dmesg, sleep, uname, poweroff, reboot, blkid, mdev, insmod,
  init, test, true, false, dd, grep, find, ps, vi, plus the network
  rescue set -- `telnetd`, `tcpsvd`, `ftpd`, `udhcpc`, `ifconfig`,
  `route` -- and `CONFIG_LFS=y` (required for this glibc toolchain --
  without it, the build fails with `BUG_off_t_size_is_misdetected`).
  The usual `defconfig` was not usable as-is: its `tc` applet fails to
  build against current kernel headers (the old CBQ qdisc structs it
  needs were removed upstream), and it isn't needed here anyway.
- `bin/<applet>` -- busybox-style symlinks to `bin/busybox` for each
  enabled applet.
- `etc/udhcpc.script` -- the callback `udhcpc` runs on `bound`/`renew`/
  `deconfig`, using only the applets this image has (`ifconfig`/
  `route`) to apply the lease, default route and DNS servers.
- `etc/passwd` -- a single `root::0:0:root:/:/bin/sh` entry. `init`
  also writes this itself if it's ever missing, so this is really just
  belt-and-suspenders.
- `sbin/mdadm` -- statically linked mdadm 4.3, cross-compiled the same
  way (`make CROSS_COMPILE=arm-linux-gnueabihf- CXFLAGS="-DNO_LIBUDEV"
  CWFLAGS="-Wno-error" mdadm.static`). `-DNO_LIBUDEV` is needed because
  there is no cross-built static `libudev.a` for armhf on the build
  host; `-Wno-error` works around GCC 15 treating mdadm 4.3's
  x86/Intel-platform-probe code (`platform-intel.c`, irrelevant on ARM)
  as a hard error over a new `-Wunterminated-string-initialization`
  warning upstream hasn't addressed yet. Stripped after linking.
- `lib/modules/*.ko` -- three kernel modules copied straight out of this
  project's own `modules.tar.xz` build output (so they always match the
  running kernel's module ABI), `insmod`-loaded by `init` before looking
  for the root array:
  - `phy-ls1024a-serdes.ko` -- the SerDes PHY driver. Needed for the
    physical SATA/USB3 link to come up at all, even though
    `CONFIG_SATA_AHCI`/`CONFIG_SATA_AHCI_PLATFORM` are built into the
    kernel (`=y`) -- AHCI being built-in doesn't make the SerDes PHY
    built-in too; it's `=m` in `ls1024a_defconfig`.
  - `usb-storage.ko` / `uas.ko` -- USB mass-storage support, so this
    image can also be booted off a USB drive as a recovery path, the
    same way pelican/monarch load their own copies of these two.
- `init` -- the actual rescue logic (see below).

## What `init` does

This image **is** the rootfs. It does not mount the board's real
Devuan/Debian install and `switch_root` into it -- it just brings the
hardware and network up, opens a remote rescue shell, and also drops
to an interactive shell on the console, so the real rootfs can be
inspected/repaired by hand (fsck, mount read-only, etc.) with no risk
of this script itself writing to it or chain-booting into it.

Structured by direct analogy with pelican-6.18's own `init` (console
-> base fs -> reboot helpers -> storage modules -> board-specific power
hack -> banner -> netrescue -> wait for disk -> partitions -> RAID
assemble -> console shell -> keep PID 1 alive), with each step either
carried over as-is, or adapted/dropped against this board's own facts:

1. **Console.** `mknod`s `/dev/console` and `/dev/null` by hand (so a
   console exists even before `/dev` is mounted) and redirects its own
   stdio to it.
2. **Base filesystems.** Mounts `/proc`, `/sys`, `/dev` (`devtmpfs`,
   falling back to `mdev -s`), creates `/tmp`, `/run`, `/dev/pts`.
3. **Reboot helpers.** Writes `/bin/reboot-now` and `/bin/poweroff-now`
   (both just `echo b`/`echo o` > `/proc/sysrq-trigger`), for a serial
   operator with no other way to signal the box.
4. **Storage modules.** `insmod`s `phy-ls1024a-serdes.ko`, then
   `usb-storage.ko`, then `uas.ko`, in that order -- no modprobe/depmod
   here, so load order matters the same way it does for pelican's own
   `phy-rtk-sata.ko` / `usb-storage.ko` / `uas.ko`: `uas` depends on
   `usb-storage`. AHCI SATA, software RAID and ext4 are all built into
   the kernel (`=y`), so they need no `insmod` of their own -- but the
   SerDes PHY the physical SATA/USB3 link needs is `=m`, same story as
   pelican needing its own SATA PHY module.
5. **USB power.** pelican/monarch poke CRT clock-gate and misc-gpio
   VBUS registers here, because RTD129x has no clk driver or gpio
   wired for USB VBUS in the kernel. This step is a deliberate no-op on
   MCG1: no equivalent register gap for LS1024A's USB controllers is
   confirmed anywhere in this project's history, so there's nothing to
   poke without inventing it.
6. **Banner.** Prints the kernel cmdline.
7. **Network rescue, unconditional.** Unlike pelican/monarch, which
   gate this behind a one-shot marker file on a FAT32 CONFIG partition,
   MCG1 has no such partition anywhere in this project (the SATA layout
   here is just partition 5=kernel, 6=initrd, 7=barebox env) -- so
   there's nothing to gate on without inventing a device node. Booting
   this partition at all is itself the opt-in step. This brings up
   `/dev/pts`, `lo`, and the first non-loopback interface in
   `/sys/class/net` (the name isn't assumed -- this board's one
   ethernet port is a built-in `fsl,ls1024a-pfe-gem` device with its
   firmware already embedded in the kernel via
   `CONFIG_EXTRA_FIRMWARE`, but whatever the kernel calls it, it's
   picked at runtime), requests a DHCP lease (10s timeout, falling back
   to static `192.168.1.222/24`), best-effort assembles the RAID so the
   data disks are reachable over the network too, and starts
   `telnetd -l /bin/sh` and `tcpsvd ... ftpd -w /` in the background --
   **unauthenticated root**, same as pelican/monarch's own rescue
   shell. Prints the telnet/ftp addresses and `/proc/partitions`.
8. **Wait for the SATA disk.** pelican/monarch wait on
   `ahci_rtd1295`'s fixed SCSI host numbers (`1:0:0:0` / `2:0:0:0`,
   since their phy driver's link-up takes several seconds). No
   equivalent fixed host number for this board's `ahci_platform` is
   confirmed anywhere in this project, so this waits generically (up to
   12s) for any `/dev/sd*` to show up in `/sys/block` instead of
   guessing a host number.
9. **Partitions.** Prints `/proc/partitions` again.
10. **RAID assemble, again.** Runs `mdadm --assemble --scan` a second
    time for the console shell's benefit (step 7's assemble was for the
    network path, which doesn't wait for the disk first). This board's
    real root filesystem is the software RAID1 array at `/dev/md0` (see
    the project `README.md`: "`md0` (the RAID1 rootfs) assembles and
    mounts"). `md1`/`md2` are other arrays on this board whose purpose
    hasn't been established anywhere in this project's history, so this
    script doesn't touch them, and doesn't guess. Neither assemble call
    ever mounts `/dev/md0` or `switch_root`'s into it -- just makes it
    available to mount by hand. If mdadm were ever dropped from a
    future build of this image, `CONFIG_MD_AUTODETECT=y` (already set
    in `ls1024a_defconfig`) would still let the kernel auto-assemble
    0.90-superblock arrays on its own before userspace ever runs.
11. **Console shell.** Drops to `/bin/sh -i` on the console. If it ever
    exits, `init` doesn't let PID 1 die (which would panic the kernel)
    -- it just sleeps in an infinite loop, so `telnetd`/`ftpd` keep
    serving regardless.

Still deliberately **not** carried over from pelican/monarch's `init`,
because it's Android-vendor-specific and doesn't apply to this board's
plain Devuan/Debian rootfs: adbd rc patching, and a vendor gzip-rootfs
fallback on a fixed partition.

## Packaging into `uRamdisk`

```sh
(cd initramfs && find . | cpio -o -H newc 2>/dev/null | pigz -11) > rootfs.cpio.gz
mkimage -A arm -O linux -T ramdisk -C none -a 0x04008000 -e 0x04008000 \
    -n initramfs -d rootfs.cpio.gz uRamdisk
```

`pigz -11` (zopfli) rather than plain `gzip -9` -- still ordinary
gzip-compatible output, just noticeably smaller (confirmed: ~3.4%
smaller payload for this image) at the cost of being much slower to
compress, which is fine for a small, infrequently-rebuilt image like
this one.

Same `mkimage` invocation (addresses, `-C none`) as documented in
`BUILDING.md` section 6 for this board's barebox, which never parses an
image's declared compression field for any image type -- the kernel's
own initrd unpacker detects the real (gzip) format from the payload's
magic bytes at unpack time.
