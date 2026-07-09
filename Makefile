# Zenctl Makefile
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
PREFIX ?= /usr/local
VERSION := $(shell git describe --abbrev=4 --dirty 2>/dev/null || echo 0.1.0)

INCS = -Iinclude
LIBS =

# ── libzenctl.so ──
# Source files are added by each domain's implementation agent.
LIB_SRC = lib/core/core.c \
	  lib/core/io.c \
	  lib/cpu/cpu.c \
	  lib/mem/mem.c \
	  lib/storage/storage.c \
	  lib/net/net.c \
	  lib/pcie/pcie.c \
	  lib/gpu/gpu.c \
	  lib/thermal/thermal.c \
	  lib/power/power.c \
	  lib/usb/util.c \
	  lib/usb/rfkill.c \
	  lib/usb/usb.c \
	  lib/usb/bt.c \
	  lib/usb/wireless.c \
	  lib/firmware/firmware.c \
	  lib/gpu/gpu.c \
	  lib/thermal/thermal.c \
	  lib/power/power.c
LIB_OBJ = $(LIB_SRC:.c=.o)

libzenctl.so: $(LIB_OBJ)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libzenctl.so.1 -o libzenctl.so.1.0.0 $^ $(LIBS)
	ln -sf libzenctl.so.1.0.0 libzenctl.so.1
	ln -sf libzenctl.so.1 libzenctl.so

lib: libzenctl.so

# ── zenctl CLI ──
CLI_SRC = cli/main.c
CLI_OBJ = $(CLI_SRC:.c=.o)

zenctl: $(CLI_OBJ) libzenctl.so
	$(CC) $(CFLAGS) -o zenctl $(CLI_OBJ) -L. -lzenctl $(LIBS)

cli: zenctl

# ── all ──
all: lib cli

# ── pkg-config ──
zenctl.pc: zenctl.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    zenctl.pc.in > zenctl.pc

pkgconfig: zenctl.pc

# ── test ──
test: zenctl-test
	LD_LIBRARY_PATH=. ./zenctl-test

zenctl-test: tests/unit/test_core.c libzenctl.so
	$(CC) $(CFLAGS) $(INCS) -o zenctl-test tests/unit/test_core.c -L. -lzenctl $(LIBS)

# ── install ──
install: all pkgconfig
	install -d $(PREFIX)/lib
	install -d $(PREFIX)/include/zenctl
	install -d $(PREFIX)/bin
	install -d $(PREFIX)/lib/pkgconfig
	install -d $(PREFIX)/share/man/man1
	install -d $(PREFIX)/share/man/man3
	install -m 755 libzenctl.so.1.0.0 $(PREFIX)/lib/
	ln -sf libzenctl.so.1.0.0 $(PREFIX)/lib/libzenctl.so.1
	ln -sf libzenctl.so.1 $(PREFIX)/lib/libzenctl.so
	install -m 644 include/zenctl/*.h $(PREFIX)/include/zenctl/
	install -m 755 zenctl $(PREFIX)/bin/
	install -m 644 zenctl.pc $(PREFIX)/lib/pkgconfig/
	install -m 644 man/zenctl.1 $(PREFIX)/share/man/man1/
	install -m 644 man/libzenctl.3 $(PREFIX)/share/man/man3/

uninstall:
	rm -f $(PREFIX)/lib/libzenctl.so*
	rm -rf $(PREFIX)/include/zenctl
	rm -f $(PREFIX)/bin/zenctl
	rm -f $(PREFIX)/lib/pkgconfig/zenctl.pc
	rm -f $(PREFIX)/share/man/man1/zenctl.1
	rm -f $(PREFIX)/share/man/man3/libzenctl.3

# ── clean ──
clean:
	rm -f $(LIB_OBJ) $(CLI_OBJ) libzenctl.so* zenctl zenctl-test zenctl.pc

# ── pattern rules ──
%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c -o $@ $<

.PHONY: all lib cli test install uninstall clean pkgconfig
