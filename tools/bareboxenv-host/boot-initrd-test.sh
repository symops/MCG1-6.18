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
