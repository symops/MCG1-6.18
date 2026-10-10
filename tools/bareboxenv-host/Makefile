# Standalone build for bareboxenv-host, out of its parent barebox tree.
#
# Upstream, this is one of several hostprogs-y tools barebox's own Kbuild
# builds alongside the bootloader itself (scripts/Makefile: `hostprogs-y +=
# bareboxenv`); scripts/bareboxenv.c #includes the rest of its own sources
# directly (see that file), so a single HOSTCC invocation is all it takes.
# HOSTCFLAGS value copied from barebox's own top-level Makefile.
CC      ?= cc
CFLAGS  := -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer

bareboxenv-host: scripts/bareboxenv.c $(wildcard lib/*.c common/*.c include/*.h)
	$(CC) $(CFLAGS) -o $@ scripts/bareboxenv.c

clean:
	rm -f bareboxenv-host

.PHONY: clean
