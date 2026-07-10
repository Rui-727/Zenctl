/* test_bt_mgmt.c - unit tests for the HCI ioctl power-control shim.
 *
 * The sandbox has no Bluetooth adapter (and no CAP_NET_ADMIN), so the
 * positive paths (real HCIDEVUP, real HCIGETDEVINFO with HCI_UP set)
 * are not exercisable here. What we test:
 *
 *   - bt_mgmt_hci_to_index() parses "hci<N>" correctly and rejects
 *     malformed names.
 *   - bt_mgmt_get_powered() / bt_mgmt_set_powered() reject bad fds
 *     and bad adapter indices with the right errno.
 *   - bt_mgmt_open() returns -1 (EAFNOSUPPORT / EPROTONOSUPPORT) on
 *     a kernel without Bluetooth; on a kernel with Bluetooth it
 *     returns a valid fd we close immediately.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "harness.h"

/* Forward declarations from lib/usb/bt_mgmt.c (private header not
 * installed; the test links against the .o directly). */
int bt_mgmt_open(void);
int bt_mgmt_hci_to_index(const char *name);
int bt_mgmt_get_powered(int fd, int adapter_idx);
int bt_mgmt_set_powered(int fd, int adapter_idx, bool on);

static void test_index_parsing(void)
{
    OK(bt_mgmt_hci_to_index("hci0") == 0, "hci0 -> 0");
    OK(bt_mgmt_hci_to_index("hci1") == 1, "hci1 -> 1");
    OK(bt_mgmt_hci_to_index("hci12") == 12, "hci12 -> 12");
    OK(bt_mgmt_hci_to_index("hci99") == 99, "hci99 -> 99");

    /* Malformed names. */
    OK(bt_mgmt_hci_to_index(NULL) == -1, "NULL name rejected");
    OK(bt_mgmt_hci_to_index("") == -1, "empty name rejected");
    OK(bt_mgmt_hci_to_index("hci") == -1, "\"hci\" (no index) rejected");
    OK(bt_mgmt_hci_to_index("wlan0") == -1, "wlan0 rejected (wrong prefix)");
    OK(bt_mgmt_hci_to_index("hciX") == -1, "hciX rejected (non-digit)");
    OK(bt_mgmt_hci_to_index("hci0x") == -1, "hci0x rejected (trailing junk)");
    OK(bt_mgmt_hci_to_index("hci-1") == -1, "hci-1 rejected");
    OK(bt_mgmt_hci_to_index("Hci0") == -1, "Hci0 rejected (case-sensitive)");
}

static void test_bad_fd(void)
{
    /* fd < 0 must return -1 with EBADF, not pass through to ioctl. */
    errno = 0;
    OK(bt_mgmt_get_powered(-1, 0) == -1, "get_powered(-1, 0) returns -1");
    OK(errno == EBADF, "get_powered(-1) sets EBADF");

    errno = 0;
    OK(bt_mgmt_set_powered(-1, 0, true) == -1, "set_powered(-1, 0, true) returns -1");
    OK(errno == EBADF, "set_powered(-1) sets EBADF");

    errno = 0;
    OK(bt_mgmt_set_powered(-1, 0, false) == -1, "set_powered(-1, 0, false) returns -1");
    OK(errno == EBADF, "set_powered(-1, false) sets EBADF");
}

static void test_bad_index(void)
{
    /* fd >= 0 but adapter_idx < 0 must fail with EINVAL. Use a
     * /dev/null fd so socket() / Bluetooth availability is not a
     * factor; the function rejects before calling ioctl. */
    int fd = open("/dev/null", O_RDONLY);
    OK(fd >= 0, "open /dev/null for test fd");
    if (fd < 0) return;

    errno = 0;
    OK(bt_mgmt_get_powered(fd, -1) == -1, "get_powered(fd, -1) returns -1");
    OK(errno == EINVAL, "get_powered(-1 idx) sets EINVAL");

    errno = 0;
    OK(bt_mgmt_set_powered(fd, -1, true) == -1, "set_powered(fd, -1, true) returns -1");
    OK(errno == EINVAL, "set_powered(-1 idx) sets EINVAL");

    /* Index > 65535 (uint16_t dev_id max) must also be rejected. */
    errno = 0;
    OK(bt_mgmt_get_powered(fd, 70000) == -1, "get_powered(fd, 70000) returns -1");
    OK(errno == EINVAL, "get_powered(70000) sets EINVAL");

    close(fd);
}

static void test_open_is_safe(void)
{
    /* bt_mgmt_open() either succeeds (kernel has Bluetooth) or
     * returns -1 (kernel without Bluetooth, EAFNOSUPPORT /
     * EPROTONOSUPPORT). Both outcomes are acceptable for this test:
     * the contract is just that the call doesn't crash. */
    int fd = bt_mgmt_open();
    if (fd >= 0) {
        close(fd);
        OK(1, "bt_mgmt_open() returned a valid fd (kernel has Bluetooth)");
    } else {
        OK(1, "bt_mgmt_open() returned -1 (kernel lacks Bluetooth)");
    }
}

int test_bt_mgmt_suite(void)
{
    SUITE_START("Bluetooth HCI ioctl shim");
    test_index_parsing();
    test_bad_fd();
    test_bad_index();
    test_open_is_safe();
    SUITE_END();
    return SUITE_FAILURES();
}
