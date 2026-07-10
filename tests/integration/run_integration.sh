#!/bin/bash
# Integration tests for zenctl. Requires root and real hardware.
# Guarded by ZENCTL_INTEGRATION=1.
# Read-only by default. Destructive writes require ZENCTL_INTEGRATION_WRITE=1.

set -euo pipefail

if [ "${ZENCTL_INTEGRATION:-0}" != "1" ]; then
    echo "Set ZENCTL_INTEGRATION=1 to run integration tests."
    exit 0
fi

BINARY="${1:-./zenctl}"
if [ ! -x "$BINARY" ]; then
    echo "zenctl binary not found at $BINARY"
    exit 1
fi

# The CLI links against libzenctl.so.1; let it find the in-tree copy
# when the script is run from the build directory before `make install`.
if [ -f "./libzenctl.so.1" ] && [ -z "${LD_LIBRARY_PATH:-}" ]; then
    export LD_LIBRARY_PATH="."
fi

PASS=0
FAIL=0

run_test() {
    local name="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "ok: $name"
        PASS=$((PASS + 1))
    else
        echo "not ok: $name"
        FAIL=$((FAIL + 1))
    fi
}

# ── Read-only tests (safe to run on any machine) ──

run_test "zenctl --version" "$BINARY" --version
run_test "zenctl --help" "$BINARY" --help
run_test "zenctl caps" "$BINARY" caps
run_test "zenctl cpu get --all" "$BINARY" cpu get --all
run_test "zenctl mem get numa" "$BINARY" mem get numa
run_test "zenctl mem get swappiness" "$BINARY" mem get swappiness
run_test "zenctl storage list" "$BINARY" storage list
run_test "zenctl net list" "$BINARY" net list
run_test "zenctl pcie list" "$BINARY" pcie list
run_test "zenctl thermal list" "$BINARY" thermal list
run_test "zenctl gpu list" "$BINARY" gpu list
run_test "zenctl power get battery" "$BINARY" power get battery
run_test "zenctl power get ac" "$BINARY" power get ac
run_test "zenctl usb list" "$BINARY" usb list
run_test "zenctl bt list" "$BINARY" bt list
run_test "zenctl wireless list" "$BINARY" wireless list
run_test "zenctl firmware dmi bios_vendor" "$BINARY" firmware dmi bios_vendor
run_test "zenctl firmware efi list" "$BINARY" firmware efi list
run_test "zenctl firmware acpi list" "$BINARY" firmware acpi list

# JSON output tests
run_test "zenctl cpu get --all --json" "$BINARY" cpu get --all --json
run_test "zenctl mem get --all --json" "$BINARY" mem get --all --json

# Dry-run tests (safe, no writes)
run_test "zenctl cpu set freq-max 3GHz --dry-run" "$BINARY" cpu set freq-max 3000000000 --dry-run
run_test "zenctl mem set swappiness 10 --dry-run" "$BINARY" mem set swappiness 10 --dry-run

# ── Write tests (require ZENCTL_INTEGRATION_WRITE=1 and root) ──

if [ "${ZENCTL_INTEGRATION_WRITE:-0}" = "1" ] && [ "$(id -u)" = "0" ]; then
    # Save current state
    OLD_SWAPPINESS=$(cat /proc/sys/vm/swappiness)

    run_test "zenctl mem set swappiness 20" "$BINARY" mem set swappiness 20 --confirm
    run_test "verify swappiness changed" sh -c "test \"$(cat /proc/sys/vm/swappiness)\" = \"20\""

    # Restore
    echo "$OLD_SWAPPINESS" > /proc/sys/vm/swappiness

    # Profile save/load
    run_test "zenctl profile save integration-test" "$BINARY" profile save integration-test
    run_test "zenctl profile list" "$BINARY" profile list
    run_test "zenctl profile load integration-test --dry-run" "$BINARY" profile load integration-test --dry-run
    run_test "zenctl profile delete integration-test" "$BINARY" profile delete integration-test
fi

echo ""
echo "Integration: $PASS pass, $FAIL fail"
exit $FAIL
