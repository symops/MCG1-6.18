# Building the MCG1 (WD My Cloud, Gen 1) kernel from a clean Debian 13

This describes building this port from scratch on a fresh Debian 13 ("trixie") machine, through packaging for testing on real hardware.

## 1. Install the toolchain and build dependencies

```sh
sudo apt update
sudo apt install -y \
    git make bc bison flex \
    gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf \
    libssl-dev libelf-dev \
    python3 cpio gzip pigz xz-utils kmod rsync u-boot-tools
```

`u-boot-tools` (for `mkimage`) **is** needed here, unlike some sibling ports in this family of repos -- this board's bootloader (barebox) genuinely expects the legacy U-Boot `uImage` header format, not a raw patched `Image`. `xz-utils` is needed too: `ls1024a_defconfig` sets `CONFIG_KERNEL_XZ` (see step 4 for why), and the kernel build invokes the host `xz` binary directly to compress the self-decompressing payload. `pigz` is for step 6 (packaging the rescue `uRamdisk`'s cpio with `pigz -11`, for a smaller image than plain `gzip -9`). Device-tree compiler is not required system-wide -- the kernel build tree compiles its own `scripts/dtc` from source.

## 2. Clone the repository

```sh
git clone https://github.com/symops/MCG1-6.18.git
cd MCG1-6.18
```

Use HTTPS, not the `git@github.com:...` SSH form, on a genuinely clean machine -- confirmed by actually trying it in a disposable VM with no prior setup: SSH clone fails twice over, first on `Host key verification failed` (no `known_hosts` entry for github.com yet) and, even past that, on having no deploy key at all. This repo is public, so HTTPS needs no authentication either way.

## 3. Get a `.config`

Unlike some sibling ports in this family of repos, `.config` **is** effectively tracked here, as `arch/arm/configs/ls1024a_defconfig` -- this file is the single source of truth for the build and is kept in sync with every real build this project ships (verified by round-tripping it through `make savedefconfig` against the actual shipped `.config` whenever it drifts). Just expand it:

```sh
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- ls1024a_defconfig
```

Don't substitute a bare/minimal defconfig here -- this repo's `ls1024a_defconfig` intentionally carries a broad driver set (USB WiFi covering WiFi 5/6/7 hardware, USB-serial for Zigbee dongles, NTFS3/iSCSI target, ksmbd, etc.) on top of the board-specific pieces; a stripped-down config would build and boot, but silently lose all of that.

## 4. Build the kernel, modules, and device trees

```sh
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-

make -j"$(nproc)" LOCALVERSION= zImage dtbs modules
```

`LOCALVERSION=` is explicit and required -- `ls1024a_defconfig` sets `# CONFIG_LOCALVERSION_AUTO is not set` (dropping the `-g<commit>` suffix), but `scripts/setlocalversion` still appends a bare `+` for an uncommitted/dirty tree unless `LOCALVERSION` is passed explicitly (even empty, as above). Without it, the kernel release string won't match the `/lib/modules/<release>/` directory the packaged `modules.tar.xz` expects (step 6), and module loading fails on vermagic mismatch.

Check the resulting version string:
```sh
cat include/config/kernel.release
```

`CONFIG_KERNEL_XZ` (not the default `CONFIG_KERNEL_GZIP`) matters for step 5's size budget -- see that step for why.

## 5. Package the boot image

```sh
cat arch/arm/boot/zImage arch/arm/boot/dts/nxp/ls/ls1024a-wdmycloud.dtb \
    > arch/arm/boot/zImage-w-dtb
mkimage -A arm -O linux -T kernel -C none -a 0x00008000 -e 0x00008000 \
    -n "Linux-$(cat include/config/kernel.release)-ls1024a-wdmycloud" \
    -d arch/arm/boot/zImage-w-dtb arch/arm/boot/uImage
```

Two things this board's barebox (2011.06.0, Dec 2013 build) requires that aren't the mkimage default:

- **Appended DTB.** This barebox has no device-tree-aware `bootm` at all -- it only ever hands off via the legacy ATAG protocol, which this kernel's `DT_MACHINE_START`-only machine definition cannot match on its own. `CONFIG_ARM_APPENDED_DTB` (already set in `ls1024a_defconfig`) makes the kernel look for a DTB concatenated directly after its own `zImage` payload -- hence the `cat` above. Plain `make uImage` does **not** do this concatenation on its own; it only wraps the bare `zImage`, which won't boot on this board.
- **A hard 10 MiB ceiling.** Barebox reads this board's kernel partition as a **fixed 10 MiB raw region**, regardless of the uImage header's own declared size -- and does not check the real image size before reading, so an oversized `uImage` is silently truncated and fails barebox's own checksum (`Verifying Checksum ... Bad Data CRC`), with no earlier warning. `CONFIG_KERNEL_XZ` (vs. the much larger `CONFIG_KERNEL_GZIP` default) is what keeps this build at ~5.3 MiB, comfortably under budget. **Always check the result against this ceiling after any config change that might pull in more built-in code:**
  ```sh
  ls -la arch/arm/boot/uImage
  ```

See `Documentation/arm/ls1024a-wdmycloud.rst` ("Boot protocol: appended DTB required" and the uImage-size section right before it) for the full story.

## 6. The board's real root filesystem is not part of this repo

This board's real root filesystem is a full Devuan/Debian install, maintained and updated independently of this kernel repository. What this repo *does* ship is `initramfs/` -- the source tree for a separate, minimal rescue/test rootfs (static busybox + mdadm, telnet/ftp network rescue), unrelated to that Devuan/Debian install; see `initramfs/README.md` for what it contains and why. Packaging it into the `uRamdisk` the initrd-based test-boot path (step 9) expects:

```sh
(cd initramfs && find . | cpio -o -H newc 2>/dev/null | pigz -11) > rootfs.cpio.gz
mkimage -A arm -O linux -T ramdisk -C none -a 0x04008000 -e 0x04008000 \
    -n initramfs -d rootfs.cpio.gz uRamdisk
```

`-A arm -O linux` are not optional despite looking like boilerplate: leaving them out doesn't error, it silently defaults to **`-A powerpc`** (confirmed by actually running the command without them -- `mkimage -l` on the result reports "PowerPC Linux RAMDisk Image" instead of "ARM Linux RAMDisk Image"). `-C none` is deliberate, not a mislabel, even though `<path-to-rootfs.cpio.gz>` is itself gzip data: this barebox never parses an image's declared compression field for *any* image type -- the same reasoning `make uImage` already relies on for the kernel image itself (see the "Kernel image size budget" section of the RST doc: `make uImage` on ARM always writes `-C none` into the header regardless of the real payload's compression, because the bootloader never acts on that field at all). Whatever decompression needs to happen is the *kernel's* job, at initrd-unpack time, which auto-detects gzip from the payload's own magic bytes -- not barebox's. The load/entry address (`0x04008000`) must match the memory window the boot script below maps the initrd into -- it is not arbitrary.

## 7. Install and package the modules

```sh
rm -rf /tmp/modinstall && mkdir -p /tmp/modinstall
make -j"$(nproc)" LOCALVERSION= modules_install INSTALL_MOD_PATH=/tmp/modinstall
( cd /tmp/modinstall/lib/modules && tar -cJf /tmp/modules.tar.xz . )
cp .config /tmp/default.config
```

Archiving from *inside* `lib/modules/` (not with `lib/modules/` as a prefix) matters here too: it makes the tar root `./<kernelrelease>/...`, so `tar -C /lib/modules -xf modules.tar.xz` on the target lands the version directory at the correct path directly.

## 8. Boot script (optional -- only for the initrd test-boot path)

The custom boot script used for step 9's initrd-based test path is packed with `bareboxenv-host`, barebox's own environment-image tool -- a **completely different** format from `mkimage` (no relation to the legacy U-Boot image format at all; comparing it against a `mkimage` magic number, as an earlier mistake in this project did, produces a false "corrupted file" diagnosis). `bareboxenv-host` is a barebox host-side build product, out of scope for this kernel-only guide -- build it from this board's own barebox 2011.06.0 source (see `symops/MCG1-3.2.26` or the vendor GPL source drop) if you need to regenerate `boot.scr`.

```sh
bareboxenv-host -s -p 568 boot.sh boot.scr
```

`boot.sh` itself (not tracked in this repo) is barebox shell, structured like this:

```sh
#!/bin/sh
sata

# Kernel: partition 5 -> the /dev/mem.uImage window (already registered
# in barebox's own /env/bin/init via "addpart /dev/mem 3M@0x3008000(uImage)").
satapart 0x3008000 5 0x5000

# initrd: partition 6 -> a new memory window (not registered by default,
# so add it here; guarded so re-running this script doesn't error on
# "partition already exists").
[ -e /dev/mem.initrd ] || addpart /dev/mem 10M@0x4008000(initrd)
satapart 0x4008000 6 0x5000

sata stop

bootargs="console=ttyS0,115200n8, init=/sbin/init swapaccount=1 panic=3"
bootm -r /dev/mem.initrd /dev/mem.uImage
```

Note there's no `root=`/`rootfstype=`/`noinitrd` in `bootargs` here: with an initrd, the kernel unpacks the cpio image as root itself.

## 9. Deploy for testing on real hardware

This board has no removable-media rescue path (no separate USB stick / eMMC fallback) -- testing means writing to the unit's own SATA disk, at partition numbers already fixed by the existing on-disk layout (inherited from the production install; this guide doesn't create partitions, only overwrites existing ones). Do this from a system that already has that disk attached and mounted/accessible as block devices -- e.g. the unit's own currently-working install, or an external machine with the disk connected.

There are two distinct boot paths on this hardware:

- **Normal boot** -- barebox's own built-in environment (`/env/bin/boot_sata`, unmodified) reads the kernel from partition 5 and boots it directly against the real installed rootfs (the `md0`/`md1`/`md2` RAID1 arrays). This is the simplest loop for an ordinary kernel-only change: just overwrite the kernel partition.
  ```sh
  dd if=uImage of=/dev/sdX5
  ```
- **Initrd test boot** -- the custom script from step 8, which boots self-contained from the kernel (partition 5) + initrd (partition 6) pair in RAM, independent of whatever state the real rootfs partitions are in. Useful when testing something that could leave the real rootfs mount unreliable. Barebox's environment partition (partition 7) runs whatever script is written there (`sataenv run 7`) -- **back up its current contents before overwriting it** with this guide's `boot.scr`, since that partition normally holds the production boot environment:
  ```sh
  dd if=/dev/sdX7 of=partition7.backup   # keep this
  dd if=uImage     of=/dev/sdX5
  dd if=uRamdisk   of=/dev/sdX6
  dd if=boot.scr   of=/dev/sdX7
  ```

In both cases, `modules.tar.xz` goes onto the real rootfs itself (mount it, or boot into it, then `tar -C /lib/modules -xf modules.tar.xz`), not onto any of the raw partitions above.

## 10. The SPI-NOR danger zone (barebox itself) is out of scope here

Everything above targets the SATA disk -- the kernel, initrd, modules, and rootfs. Barebox itself, and its own environment, live on a **separate SPI-NOR flash chip**, not the SATA disk. This repo does include a real fix for that flash (a write-hang bug in the DesignWare SPI controller driver, `drivers/spi/spi-dw-core.c` -- see the RST doc's "SPI-NOR boot flash access from Linux" section), developed and confirmed working from *within* a booted Linux on this same board. This guide does not cover writing to that flash or to barebox itself: a mistake there risks the bootloader that makes any of the above recoverable in the first place, and should only be attempted with a hardware-programmer-based recovery plan already in place, independent of anything this kernel can do for you after the fact.
