# `bareboxenv-host`

This is barebox's own environment-image packing tool -- not a `mkimage`-format
tool at all (a completely different format; see `BUILDING.md` step 8). It
reads a directory (or, as used here, a single file) and packs it into a
barebox env-filesystem image (`ENVFS_MAGIC` superblock + per-file inodes),
the format `boot.scr` on this board's barebox env partition (partition 7)
actually is.

## Provenance

Vendored from upstream barebox at tag `v2011.06.0` (the same release this
board's barebox is built from -- see `BUILDING.md` step 8 and
`Documentation/arm/ls1024a-wdmycloud.rst`, which identify it as
"2011.06.0-svn10510, Dec 2013 build"). The files here are an unmodified copy
of `scripts/bareboxenv.c` plus everything it `#include`s directly
(`lib/recursive_action.c`, `lib/crc32.c`, `lib/make_directory.c`,
`include/envfs.h`, `include/environment.h`, `common/environment.c`), with
the same relative directory layout so those `#include "../lib/...")`
paths resolve unchanged. GPLv2, per each file's own header.

This is the *generic* upstream source, not WD's own exact vendor-patched
tree (which this project doesn't have a copy of -- see "What this is NOT"
below for why that distinction matters less than it sounds). `bareboxenv.c`
and everything it pulls in build entirely inside `#ifdef __BAREBOX__` guards
for anything barebox-internal (see the top of `common/environment.c`:
"Important: This file will also be used on the host... so do not add any
new barebox related functions here!") -- it's designed upstream to be
generic host tooling, unrelated to any board-specific vendor patching.

## Building

```sh
make -C tools/bareboxenv-host
```

Plain host `cc`, no cross-compiler -- this runs on the build machine, not
the target board.

## What this is NOT

**This does not rebuild this board's real, production `boot.scr`.**
Confirmed by actually extracting the strings of the `boot.scr` already
shipped in this project's GitHub releases (and sitting in `build/mcg1/`):
it reads button state (`get_button_status`, `btn_status`) and passes extra
`bootargs` (`mac_addr`, `model`, `serial`, `board_test`, `btn_status`) that
`boot-initrd-test.sh` in this directory simply doesn't have -- the two are
different scripts for different purposes, not two copies of the same
content. The *production* script's own source isn't tracked anywhere in
this project (same situation as the busybox/mdadm binaries in
`initramfs/` -- vendored as a built artifact, not source); only this
simplified **test-path** script (`BUILDING.md` step 8) is.

So: this tool builds a `boot.scr` for the *optional initrd test boot path*
only (`BUILDING.md` step 9, "Initrd test boot") -- temporarily replacing
partition 7's contents to test `uImage`/`uRamdisk` in isolation, with the
real production environment backed up first and restored afterward, exactly
as step 9 already describes. It must never be written to `build/mcg1/boot.scr`
or published as a release's `boot.scr` in place of the real one.
