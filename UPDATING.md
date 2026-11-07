# Updating the kernel in place, and recovering without a serial console

`BUILDING.md` ends with the disk pulled out of the NAS and written on a PC. Once the board runs Linux from its own
SATA root filesystem that is no longer necessary: the kernel partition can be rewritten from the running system, and
if the new kernel does not come back there is a network-only way into the box (no serial console, no disk pull)
through a feature of the stock WD barebox. Both were used for every kernel iteration of the PFE / L2-cache work in
this tree, on a WD My Cloud gen1 with no serial console attached.

## 1. Update the kernel from the running system

Barebox loads the first 10 MiB of partition 5 as a raw `uImage` (see `BUILDING.md`, step 5). On a stock-partitioned
disk that is `/dev/sda5`, 95 MiB, and nothing else lives there.

```sh
# keep a copy of what boots today -- on the data partition, where the recovery image can reach it
mkdir -p /srv/data/_newfw
dd if=/dev/sda5 of=/srv/data/_newfw/sda5-known-good.img bs=1M count=10

# write the new image and verify it byte for byte before rebooting
dd if=uImage of=/dev/sda5 bs=1M conv=fsync
cmp -n "$(stat -c %s uImage)" uImage /dev/sda5 && echo OK
sync
reboot
```

The board is back on the network about 100 s after `reboot` (roughly 60 s of barebox + kernel, 40 s of userspace).
If it answers `ping` but not SSH within three minutes, give it another minute; if it never answers `ping`, go to
section 3.

### Modules and `vermagic`

Modules in `/lib/modules/<release>/` must match the running kernel's `vermagic` string, which encodes more than the
version: it changes with `CONFIG_PREEMPT*`, `CONFIG_SMP` and `CONFIG_MODULE_UNLOAD`. A kernel built with the same
`.config` can reuse the installed modules; after a config change of that kind (for example switching to
`PREEMPT_NONE`) install the new `modules.tar.xz` from `BUILDING.md` step 7 before rebooting, or the next boot comes
up with `systemd-modules-load.service` failed and no `nfsd`, `ipv6`, USB storage, etc.:

```sh
mv /lib/modules/6.18.59 /lib/modules/6.18.59.old
mkdir /lib/modules/6.18.59
tar -xJf modules.tar.xz -C /lib/modules      # the archive contains ./6.18.59/...
depmod -a 6.18.59
```

`xz` is not part of a minimal Debian rootfs; repack as `.tar.gz` on the build host if needed.

### Things that are easy to get wrong

- **10 MiB ceiling.** Check `ls -la uImage` after every config change; barebox truncates silently.
- **Image name.** `mkimage -n` is limited to 32 characters; longer names are cut, which only matters if something
  reads the header back (a status page, `dumpimage`).
- **Keep the known-good image on `/srv/data`**, not on the root filesystem: the recovery image below can mount the
  data partition directly, mounting the RAID1 root is one more step.

## 2. Update the rescue `uRamdisk` and `boot.scr`

Neither matters for day-to-day operation the way the kernel does -- the board's normal boot doesn't depend on either
changing -- but both live on the SATA disk too, so the same from-the-running-system approach as section 1 applies,
with partition-specific caveats below.

### `uRamdisk` (partition 6)

Same fixed-size raw-region convention as the kernel partition (`BUILDING.md` step 6 and step 5's "A hard 10 MiB
ceiling" note): barebox reads a fixed 10 MiB region starting at the partition, regardless of the real image size.

```sh
mkdir -p /srv/data/_newfw
dd if=/dev/sda6 of=/srv/data/_newfw/sda6-known-good.img bs=1M count=10

dd if=uRamdisk of=/dev/sda6 bs=1M conv=fsync
cmp -n "$(stat -c %s uRamdisk)" uRamdisk /dev/sda6 && echo OK
sync
```

This only matters for the **initrd test boot** path (`BUILDING.md` step 9) -- the board's normal boot (direct
`root=/dev/md0`, no initrd) never reads this partition on its own, so there is no reboot to wait on here; the new
image just sits there until something actually boots off it.

### `boot.scr` (partition 7)

This is barebox's own environment image, not a fixed-size raw-region read like the two partitions above -- its own
superblock carries the real size, so there is no 10 MiB (or any other) ceiling to watch here.

**Back this up before writing anything new.** Unlike partitions 5/6, this one already holds the board's real
production boot environment in normal operation -- it is not something this repo's own build produces by default.
The script `tools/bareboxenv-host/boot.sh` in this repo (built per `BUILDING.md` step 8) is the simplified
**test-path** script for the initrd test boot above, not a copy of the production one: the real production script
also handles button state (`get_button_status`/`btn_status`) and passes extra `bootargs`
(`mac_addr`/`model`/`serial`/`board_test`) that the test-path one does not -- see `tools/bareboxenv-host/README.md`
for the full story.

```sh
dd if=/dev/sda7 of=/srv/data/_newfw/sda7-production.img   # keep this -- see above
dd if=boot.scr of=/dev/sda7 conv=fsync
sync
```

Restore the production script the same way once done testing: `dd if=/srv/data/_newfw/sda7-production.img
of=/dev/sda7`.

### Things that are easy to get wrong

- **`boot.scr` overwrites the production boot environment**, not something inert -- confirm this unit is meant to
  run the test-path script before writing it, and keep the backup from above.
- **`uRamdisk`'s vendored modules are vermagic-pinned.** The three `.ko` files inside it
  (`phy-ls1024a-serdes`/`usb-storage`/`uas`) must match the kernel it boots alongside -- update `uImage` and
  `uRamdisk` together from the same release/build, not a kernel from one and a `uRamdisk` built for another (see
  `initramfs/README.md`).
- **10 MiB ceiling applies to `uRamdisk` too**, same as `uImage` -- check `ls -la uRamdisk` after any change to
  `initramfs/`.

## 3. Recovery without a serial console

The stock WD barebox (2011.06.0) has a network recovery hook: during the first seconds after power-on it listens for
an ICMP echo request whose payload is the string `WD-ICMP-BEACON`, sent to the board's MAC address. On receipt it
runs DHCP, fetches `startup.sh` from the TFTP server named by DHCP and executes it; the WD recovery flow then TFTPs a
recovery image (`recovery`, a uImage with an embedded initramfs) and boots it. Nothing on the SATA disk is involved,
so this works no matter what is on `sda5`.

A ready-made recovery image exists: the "Fox_exe" `Recovery.zip` for the WD My Cloud gen1 (kernel 3.2.68, busybox
with `telnetd`, `tftp`, `mdadm`, `xz`, `tar`; telnet login `root`, password `mycloud`). It is widely mirrored on the
WD community forums; keep a copy next to your kernel backups.

### Server side (any Linux box on the same L2 segment)

`dnsmasq` as DHCP + TFTP server, bound to the LAN interface. The board asks for `startup.sh` **at the TFTP root**,
not inside the `root-path`/option-17 directory, so place the files at the root:

```
# dnsmasq.conf
port=0
interface=lan0
bind-interfaces
dhcp-range=192.168.12.0,static
dhcp-host=00:90:a9:44:13:0a,192.168.12.108,2m   # the NAS MAC and the address to hand it
dhcp-option=3,192.168.12.254
dhcp-option=17,"data"
dhcp-boot=startup.sh,,192.168.12.12              # this host's address
enable-tftp
tftp-root=/tftp
log-dhcp
log-facility=-
```

```
/tftp/startup.sh    # from Recovery.zip
/tftp/recovery      # from Recovery.zip
```

Run it (`dnsmasq -d -C dnsmasq.conf`, or in a container with `--network host`), then send the beacon continuously
while power-cycling the NAS:

```sh
nping -e lan0 --dest-mac 00:90:a9:44:13:0a -c 900 --delay 1s --icmp --icmp-type echo --icmp-code 0 \
      --data-string WD-ICMP-BEACON 255.255.255.255
```

`nping` is part of `nmap`. Power the NAS off and on; within about 30 s the dnsmasq log shows the DHCP lease and the
TFTP transfers, and a minute later the recovery image answers on telnet port 23.

### Inside the recovery image

```sh
telnet 192.168.12.108            # root / mycloud
mdadm --assemble --scan          # optional: brings up /dev/md0 (the RAID1 root)
mkdir -p /mnt/data && mount /dev/sda4 /mnt/data
dd if=/mnt/data/_newfw/sda5-known-good.img of=/dev/sda5 bs=1M count=10
sync
reboot -f
```

The busybox shell in that image does not like `&` or `;` in some constructs; keep commands simple, and for anything
long write a script to `/tmp` and run it with `nohup`.

### If even that is not enough

The disk can still be pulled and written on a PC (`BUILDING.md`, step 9); barebox itself in the SPI NOR is untouched
by any of the above, which is what makes the beacon recovery possible in the first place.
