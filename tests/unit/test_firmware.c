/* test_firmware.c - firmware / BIOS domain unit tests against a mock tree.
 *
 * Covers:
 *   - zenctl_fw_dmi_get against /sys/class/dmi/id/
 *   - zenctl_fw_efi_enumerate against /sys/firmware/efi/efivars/
 *   - zenctl_fw_efi_get parsing the 4-byte attribute prefix + payload
 *
 * The efivars_mounted() helper in firmware.c uses statfs() and checks
 * for EFIVARFS_MAGIC. The mock_preload.c shim lies about f_type for
 * /sys/firmware/efi/efivars so the EFI tests can run on tmpfs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_dmi_get(void)
{
    /* /sys/class/dmi/id/bios_vendor with leading space (kernel pads
     * some DMI strings). The library strips trailing newlines but
     * preserves interior and leading whitespace. */
    mock_sysfs_create_dir("sys/class/dmi/id");
    mock_sysfs_create_file("sys/class/dmi/id/bios_vendor", " Lenovo");
    mock_sysfs_create_file("sys/class/dmi/id/bios_version", "N1VET50W");
    mock_sysfs_create_file("sys/class/dmi/id/product_name", "ThinkPad X1");

    char *s = NULL;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_fw_dmi_get("bios_vendor", &s, &err);
    OK(rc == 0, "fw_dmi_get(\"bios_vendor\") returns 0");
    OK(s && strcmp(s, " Lenovo") == 0,
       "fw_dmi_get returns \" Lenovo\" with leading space preserved");
    free(s);

    memset(&err, 0, sizeof(err));
    s = NULL;
    OK(zenctl_fw_dmi_get("bios_version", &s, &err) == 0,
       "fw_dmi_get(\"bios_version\") returns 0");
    OK(s && strcmp(s, "N1VET50W") == 0,
       "fw_dmi_get returns \"N1VET50W\"");
    free(s);

    /* reject unknown field name (whitelist) */
    memset(&err, 0, sizeof(err));
    s = NULL;
    rc = zenctl_fw_dmi_get("not_a_field", &s, &err);
    OK(rc == -1, "fw_dmi_get(\"not_a_field\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "fw_dmi_get(\"not_a_field\") sets ZENCTL_ERR_EINVAL");

    /* reject NULL */
    memset(&err, 0, sizeof(err));
    rc = zenctl_fw_dmi_get(NULL, &s, &err);
    OK(rc == -1, "fw_dmi_get(NULL) rejected");

    /* missing file -> ENOENT */
    memset(&err, 0, sizeof(err));
    s = NULL;
    rc = zenctl_fw_dmi_get("board_serial", &s, &err);
    OK(rc == -1, "fw_dmi_get(\"board_serial\") returns -1 (no fixture file)");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "fw_dmi_get(missing file) sets ZENCTL_ERR_ENOENT");
}

static void test_efi_enumerate_and_get(void)
{
    /* Build /sys/firmware/efi/efivars/ with two fake variables.
     * Filenames are "<NAME>-<GUID>" with GUID = 36 chars
     * (8-4-4-4-12 hex, separated by dashes). File contents are
     * 4-byte LE attributes + payload. */
    mock_sysfs_create_dir("sys/firmware/efi/efivars");

    /* BootOrder-8be4df61-93ca-11d2-aa0d-00e098032b8c
     * attributes = 0x07 (BS|RT|NV)
     * payload = a 4-byte uint16 list of two entries: 0x0001, 0x0002 */
    uint8_t boot_order[4 + 4] = {
        0x07, 0x00, 0x00, 0x00,    /* attributes LE */
        0x01, 0x00, 0x02, 0x00,    /* payload */
    };
    mock_sysfs_create_file_bin(
        "sys/firmware/efi/efivars/"
        "BootOrder-8be4df61-93ca-11d2-aa0d-00e098032b8c",
        (const char *)boot_order, sizeof(boot_order));

    /* Timeout-8be4df61-93ca-11d2-aa0d-00e098032b8c
     * attributes = 0x06 (BS|RT), payload = 0x00000002 (uint32) */
    uint8_t timeout[4 + 4] = {
        0x06, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
    };
    mock_sysfs_create_file_bin(
        "sys/firmware/efi/efivars/"
        "Timeout-8be4df61-93ca-11d2-aa0d-00e098032b8c",
        (const char *)timeout, sizeof(timeout));

    /* enumerate */
    zenctl_efi_var_t *list = NULL;
    int n = 0;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_fw_efi_enumerate(&list, &n, &err);
    OK(rc == 0, "fw_efi_enumerate returns 0");
    OK(n == 2, "fw_efi_enumerate returns 2 variables");
    if (rc == 0 && n > 0) {
        /* list is sorted by name? Not guaranteed by the lib (it uses
         * readdir order). Just check that both names appear. */
        bool have_boot = false, have_timeout = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(list[i].name, "BootOrder") == 0) {
                have_boot = true;
                OK(list[i].attributes == 0x07,
                   "BootOrder attributes == 0x07");
                OK(list[i].data_size == 4,
                   "BootOrder data_size == 4 (excludes 4-byte attr prefix)");
            }
            if (strcmp(list[i].name, "Timeout") == 0) {
                have_timeout = true;
                OK(list[i].attributes == 0x06,
                   "Timeout attributes == 0x06");
                OK(list[i].data_size == 4, "Timeout data_size == 4");
            }
            OK(strcmp(list[i].guid,
                      "8be4df61-93ca-11d2-aa0d-00e098032b8c") == 0,
               "EFI variable GUID is the global EFI var GUID");
        }
        OK(have_boot, "enumerate found BootOrder");
        OK(have_timeout, "enumerate found Timeout");
    }
    free(list);

    /* efi_get on BootOrder: returns the payload (4 bytes), not the
     * 4-byte attribute prefix. */
    uint8_t *data = NULL;
    size_t sz = 0;
    memset(&err, 0, sizeof(err));
    rc = zenctl_fw_efi_get("BootOrder",
                           "8be4df61-93ca-11d2-aa0d-00e098032b8c",
                           &data, &sz, &err);
    OK(rc == 0, "fw_efi_get(\"BootOrder\") returns 0");
    OK(sz == 4, "fw_efi_get returns 4-byte payload (prefix stripped)");
    if (rc == 0 && sz >= 2) {
        /* payload is little-endian uint16 list 0x0001 0x0002 */
        uint16_t v0 = (uint16_t)(data[0] | (data[1] << 8));
        uint16_t v1 = (uint16_t)(data[2] | (data[3] << 8));
        OK(v0 == 0x0001, "BootOrder[0] == 0x0001");
        OK(v1 == 0x0002, "BootOrder[1] == 0x0002");
    }
    free(data);

    /* efi_get on a missing variable -> ENOENT */
    memset(&err, 0, sizeof(err));
    data = NULL; sz = 0;
    rc = zenctl_fw_efi_get("NoSuchVar",
                           "8be4df61-93ca-11d2-aa0d-00e098032b8c",
                           &data, &sz, &err);
    OK(rc == -1, "fw_efi_get(\"NoSuchVar\") returns -1");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "fw_efi_get(missing) sets ZENCTL_ERR_ENOENT");

    /* efi_get rejects bad name (contains '-') and bad GUID */
    memset(&err, 0, sizeof(err));
    rc = zenctl_fw_efi_get("Bad-Name",
                           "8be4df61-93ca-11d2-aa0d-00e098032b8c",
                           &data, &sz, &err);
    OK(rc == -1, "fw_efi_get(\"Bad-Name\") rejected (name contains '-')");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "fw_efi_get(\"Bad-Name\") sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    rc = zenctl_fw_efi_get("BootOrder", "not-a-guid",
                           &data, &sz, &err);
    OK(rc == -1, "fw_efi_get(bad GUID) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "fw_efi_get(bad GUID) sets ZENCTL_ERR_EINVAL");
}

int test_firmware_suite(void)
{
    SUITE_START("firmware domain");
    test_dmi_get();
    test_efi_enumerate_and_get();
    SUITE_END();
    return SUITE_FAILURES();
}
