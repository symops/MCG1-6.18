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
  uses enabled (ash/sh, mount, umount, switch_root, mkdir, mknod, ls,
  cat, echo, dmesg, sleep, uname, poweroff, reboot, blkid, mdev, insmod,
  init, test, true, false, dd, grep, find, ps, vi), plus `CONFIG_LFS=y`
  (required for this glibc toolchain -- without it, the build fails with
  `BUG_off_t_size_is_misdetected`). The usual `defconfig` was not usable
  as-is: its `tc` applet fails to build against current kernel headers
  (the old CBQ qdisc structs it needs were removed upstream), and it
  isn't needed here anyway.
- `bin/<applet>` -- busybox-style symlinks to `bin/busybox` for each
  enabled applet.
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

1. Mounts `/proc`, `/sys`, and `/dev` (via `devtmpfs`, falling back to
   `mdev -s` if that fails).
2. `insmod`s the three modules above (best-effort -- a failure prints a
   warning but isn't fatal, since a missing USB module just means no
   USB-attached rescue media, not a dead image).
3. If `/sbin/mdadm` is present, runs `mdadm --assemble --scan` to
   assemble the board's software RAID. If mdadm were ever dropped from
   a future build of this image, `CONFIG_MD_AUTODETECT=y` (already set
   in `ls1024a_defconfig`) would still let the kernel auto-assemble
   0.90-superblock arrays on its own before userspace ever runs --
   mdadm here is present, but this is why the script doesn't treat a
   missing mdadm as fatal either.
4. Waits (up to 10 seconds) for `/dev/md0` to appear. This board's real
   root filesystem is the software RAID1 array at `/dev/md0` (see the
   project `README.md`: "`md0` (the RAID1 rootfs) assembles and
   mounts"). `md1`/`md2` are other arrays on this board whose purpose
   hasn't been established anywhere in this project's history, so this
   script doesn't touch them, and doesn't guess.
5. Mounts `/dev/md0` as `ext4`, **read-write** (not read-only): the
   handoff target is the real system's own `/sbin/init`, which expects
   to keep booting a normal read-write root and will fsck/remount it
   itself as needed -- mounting read-only here would just make the real
   init's own startup fail instead of helping anything.
6. On success, `exec switch_root /mnt/root /sbin/init` -- hands off to
   the real system exactly as if it had booted normally.
7. On any failure along the way (no `/dev/md0`, mount failure, no
   `/sbin/init` on it), prints why, and falls back to an interactive
   `/bin/sh` on the console.

Deliberately **not** carried over from pelican/monarch's `init`, because
it's either not applicable to this board or would require inventing
unconfirmed hardware facts:
- Android-vendor-specific logic (adbd rc patching, a vendor gzip-rootfs
  fallback on a fixed partition) -- this isn't an Android-derived
  rootfs.
- USB VBUS/clock register pokes -- no such requirement has been
  established for this board.
- Network rescue over telnet/ftp via a CONFIG partition -- would need a
  confirmed device node for a FAT config partition on this board, which
  this project's history doesn't have.

## Packaging into `uRamdisk`

```sh
(cd initramfs && find . | cpio -o -H newc 2>/dev/null | gzip -9) > rootfs.cpio.gz
mkimage -A arm -O linux -T ramdisk -C none -a 0x04008000 -e 0x04008000 \
    -n initramfs -d rootfs.cpio.gz uRamdisk
```

Same `mkimage` invocation (addresses, `-C none`) as documented in
`BUILDING.md` section 6 for this board's barebox, which never parses an
image's declared compression field for any image type -- the kernel's
own initrd unpacker detects the real (gzip) format from the payload's
magic bytes at unpack time.
