/* test_main.c - aggregates all libzenctl unit test suites.
 *
 * Each suite is in its own translation unit (test_<domain>.c). This
 * file declares the suite entry points and runs them in order,
 * aggregating the failure counts. Exits non-zero if any suite failed.
 *
 * The mock tree is initialised once at startup and torn down at exit:
 * every suite shares the same ZENCTL_SYSFS_PREFIX and writes to the
 * same fixture, which is fine because each suite creates the paths it
 * needs (and overwrites shared paths if necessary). For suites that
 * need a clean tree, mock_sysfs_cleanup() + mock_sysfs_init() can be
 * called between suites.
 */
#include <stdio.h>
#include <stdlib.h>

#include "mock_sysfs.h"

int test_cpu_suite(void);
int test_mem_suite(void);
int test_storage_suite(void);
int test_thermal_suite(void);
int test_net_suite(void);
int test_power_suite(void);
int test_pcie_suite(void);
int test_usb_suite(void);
int test_nvml_suite(void);
int test_bt_mgmt_suite(void);
int test_firmware_suite(void);

int main(void)
{
    if (mock_sysfs_init() != 0) {
        fprintf(stderr, "mock_sysfs_init failed\n");
        return 2;
    }
    printf("ZENCTL_SYSFS_PREFIX=%s\n", mock_sysfs_prefix());

    int total_fail = 0;
    total_fail += test_cpu_suite();
    total_fail += test_mem_suite();
    total_fail += test_storage_suite();
    total_fail += test_thermal_suite();
    total_fail += test_net_suite();
    total_fail += test_power_suite();
    total_fail += test_pcie_suite();
    total_fail += test_usb_suite();
    total_fail += test_nvml_suite();
    total_fail += test_bt_mgmt_suite();
    total_fail += test_firmware_suite();

    mock_sysfs_cleanup();

    printf("\n=== TOTAL: %d failure(s) ===\n", total_fail);
    return total_fail > 0 ? 1 : 0;
}
