# Zenctl Makefile
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
PREFIX ?= /usr/local

# Library version. Falls back to 0.1.0 when git describe fails.
# ZENCTL_VERSION_MAJOR/MINOR/PATCH are injected as -D defines so the
# library can report its version via zenctl_version() without re-parsing
# the git tag at runtime.
ZENCTL_VERSION_MAJOR ?= 0
ZENCTL_VERSION_MINOR ?= 1
ZENCTL_VERSION_PATCH ?= 0
VERSION := $(shell git describe --abbrev=4 --dirty 2>/dev/null || echo $(ZENCTL_VERSION_MAJOR).$(ZENCTL_VERSION_MINOR).$(ZENCTL_VERSION_PATCH))
VERSION_DEFS := -DZENCTL_VERSION_MAJOR=$(ZENCTL_VERSION_MAJOR) \
                -DZENCTL_VERSION_MINOR=$(ZENCTL_VERSION_MINOR) \
                -DZENCTL_VERSION_PATCH=$(ZENCTL_VERSION_PATCH)
CFLAGS += $(VERSION_DEFS)

# Optional Bluetooth development headers. When libbluetooth-dev is
# installed, bt_mgmt.c uses the system <bluetooth/hci.h>; otherwise it
# falls back to local definitions of the kernel ABI (the build
# succeeds either way). Other agents' domains ignore this flag.
HAVE_BLUETOOTH := $(shell printf '#include <bluetooth/hci.h>\n' | \
    $(CC) -E -x c - >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(HAVE_BLUETOOTH),1)
CFLAGS += -DHAVE_BLUETOOTH
endif

# `make` with no args builds both libzenctl.so and the zenctl binary.
.DEFAULT_GOAL := all

INCS = -Iinclude -Icli

# -ldl: dlopen() for NVML (lib/gpu/nvml.c) and the mock preload shim.
LIBS = -ldl

# -- libzenctl.so --
# Source files that are still being implemented by other agents are
# listed via $(wildcard) so the build succeeds whether or not they
# are present in the working tree. Once a file is stable it can be
# moved into the explicit list.
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
          lib/usb/nl80211.c \
          lib/usb/usb.c \
          lib/usb/bt.c \
          lib/usb/wireless.c \
          lib/firmware/firmware.c \
          $(wildcard lib/gpu/nvml.c) \
          $(wildcard lib/usb/bt_mgmt.c)
LIB_OBJ = $(LIB_SRC:.c=.o)

libzenctl.so: $(LIB_OBJ)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libzenctl.so.1 -o libzenctl.so.1.0.0 $^ $(LIBS)
	ln -sf libzenctl.so.1.0.0 libzenctl.so.1
	ln -sf libzenctl.so.1 libzenctl.so

lib: libzenctl.so

# -- zenctl CLI --
CLI_SRC = cli/main.c \
          cli/output.c \
          cli/cmd_util.c \
          cli/profile.c \
          cli/cmd/cpu.c \
          cli/cmd/mem.c \
          cli/cmd/storage.c \
          cli/cmd/net.c \
          cli/cmd/pcie.c \
          cli/cmd/gpu.c \
          cli/cmd/thermal.c \
          cli/cmd/power.c \
          cli/cmd/usb.c \
          cli/cmd/bt.c \
          cli/cmd/wireless.c \
          cli/cmd/firmware.c \
          cli/cmd/caps.c
CLI_OBJ = $(CLI_SRC:.c=.o)

zenctl: $(CLI_OBJ) libzenctl.so
	$(CC) $(CFLAGS) -o zenctl $(CLI_OBJ) -L. -lzenctl -Wl,-rpath,$(PREFIX)/lib $(LIBS)

cli: zenctl

# -- all --
all: lib cli

# -- pkg-config --
zenctl.pc: zenctl.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
            -e 's|@VERSION@|$(VERSION)|g' \
            zenctl.pc.in > zenctl.pc

pkgconfig: zenctl.pc

# -- test --
#
# zenctl-test is the aggregated unit-test binary. TAP-style output,
# non-zero exit on any failure. libzenctl_mockpreload.so is an
# LD_PRELOAD shim that redirects access/opendir/readlink/stat/open
# on /sys/ and /proc/ paths to the ZENCTL_SYSFS_PREFIX fixture tree.
# The shim is a no-op when ZENCTL_SYSFS_PREFIX is unset.
#
# TEST_SRC lists every suite that has been wired into the runner.
# $(wildcard) is used for suites that are still being implemented by
# other agents (test_wireless, test_errors, test_caps, test_kv_api):
# if the file is present, it is linked in; if not, the build skips it
# instead of failing. Once a suite is stable it can be moved into the
# explicit list.

TEST_SRC = tests/unit/test_main.c \
           tests/unit/mock_sysfs.c \
           tests/unit/test_cpu.c \
           tests/unit/test_mem.c \
           tests/unit/test_storage.c \
           tests/unit/test_thermal.c \
           tests/unit/test_net.c \
           tests/unit/test_power.c \
           tests/unit/test_pcie.c \
           tests/unit/test_usb.c \
           $(wildcard tests/unit/test_wireless.c) \
           tests/unit/test_firmware.c \
           $(wildcard tests/unit/test_nvml.c) \
           $(wildcard tests/unit/test_bt_mgmt.c) \
           $(wildcard tests/unit/test_errors.c) \
           $(wildcard tests/unit/test_caps.c) \
           $(wildcard tests/unit/test_kv_api.c)

zenctl-test: $(TEST_SRC) libzenctl.so libzenctl_mockpreload.so
	$(CC) $(CFLAGS) $(INCS) -o zenctl-test $(TEST_SRC) -L. -lzenctl $(LIBS)

libzenctl_mockpreload.so: tests/unit/mock_preload.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

test: zenctl-test
	LD_LIBRARY_PATH=. LD_PRELOAD=./libzenctl_mockpreload.so ./zenctl-test

# `make check` is the conventional alias for `make test`.
check: test

# Verbose TAP run: identical to `test` but prints an explicit banner
# with the environment so the per-test "ok N: ..." / "not ok N: ..."
# lines are easy to find in CI logs.
test-verbose: zenctl-test
	@echo "=== zenctl-test verbose run ==="
	@echo "binary:    ./zenctl-test"
	@echo "preload:   ./libzenctl_mockpreload.so"
	@echo "prefix:    $${ZENCTL_SYSFS_PREFIX:-<unset>}"
	LD_LIBRARY_PATH=. LD_PRELOAD=./libzenctl_mockpreload.so ./zenctl-test

# Coverage build: rebuild with --coverage, run the tests, then print a
# per-source-file summary. Requires gcov (ships with gcc).
coverage:
	@command -v gcov >/dev/null 2>&1 || { \
                echo "gcov is not installed. Install it (e.g. 'apt install gcov'"; \
                echo "or 'dnf install gcc-gcov') to see coverage data."; \
                exit 0; \
	}
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=gnu11 -Wall -Wextra -O0 -g --coverage -fprofile-arcs -ftest-coverage" test
	@echo ""
	@echo "=== Coverage summary ==="
	@for f in lib/*/*.o; do \
                gcov -n "$$f" 2>/dev/null \
                  | awk -F"'" '/^File:/{file=$$2} /^Lines executed:/{print file": "$$0}'; \
	done | sort || true
	@echo ""
	@echo "Per-file .gcov reports are alongside the .o files (e.g. lib/core/core.c.gcov)."

# smoke: legacy test_core.c against the real /sys surface. Not part of `make test`.
zenctl-smoke: tests/unit/test_core.c libzenctl.so
	$(CC) $(CFLAGS) $(INCS) -o zenctl-smoke tests/unit/test_core.c -L. -lzenctl $(LIBS)

smoke: zenctl-smoke
	LD_LIBRARY_PATH=. ./zenctl-smoke

# -- install --
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
	-ldconfig 2>/dev/null || true

uninstall:
	rm -f $(PREFIX)/lib/libzenctl.so*
	rm -rf $(PREFIX)/include/zenctl
	rm -f $(PREFIX)/bin/zenctl
	rm -f $(PREFIX)/lib/pkgconfig/zenctl.pc
	rm -f $(PREFIX)/share/man/man1/zenctl.1
	rm -f $(PREFIX)/share/man/man3/libzenctl.3
	-ldconfig 2>/dev/null || true

# -- clean --
clean:
	rm -f $(LIB_OBJ) $(CLI_OBJ) libzenctl.so* zenctl zenctl-test \
              zenctl-smoke libzenctl_mockpreload.so zenctl.pc

# -- pattern rules --
%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c -o $@ $<

.PHONY: all lib cli test check test-verbose coverage smoke install uninstall clean pkgconfig
