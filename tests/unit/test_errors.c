/* test_errors.c - edge-case / error-path coverage for every domain.
 *
 * Each section covers one category of misuse:
 *   1. NULL argument to every zenctl_*_open()
 *   2. Non-existent path/name to every zenctl_*_open()
 *   3. NULL out parameter to zenctl_*_get_*()
 *   4. NULL handle to zenctl_*_get_*()
 *   5. Out-of-range value to zenctl_*_set_*()
 *   6. NULL value to zenctl_*_set_*()
 *   7. Permission boundary: write to a read-only fixture file (non-root)
 *   8. Generic key-value API with malformed keys
 *
 * The "string return functions with a zero-size buffer" requirement
 * does not apply to the typed API: every char**-returning function
 * heap-allocates its output (caller frees). The shared internal helper
 * zenctl__read_file_string does reject bufsize==0, but that path is
 * never reachable through the public API. We cover the equivalent
 * guarantee (NULL out -> EINVAL) in section 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

/* Build a full set of fixtures for one CPU (cpu2), one block device
 * (sdz), one NIC (eth9), one PCI device (0000:07:00.0), one thermal
 * zone (thermal_zone0), one USB device (3-1), and the /proc/sys/vm
 * tunables. These intentionally use names that no other suite touches
 * so this file is independent of test ordering. */
static void build_fixtures(void)
{
    /* CPU 2 with cpufreq subtree (mirrors what test_cpu.c builds for
     * cpu0, so the same getters/setters work). */
    mock_sysfs_create_dir("sys/devices/system/cpu/cpu2/cpufreq");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu2/cpufreq/scaling_governor",
        "performance");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu2/cpufreq/scaling_available_governors",
        "performance powersched");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq",
        "3600000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq",
        "800000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu2/cpufreq/cpuinfo_min_freq",
        "800000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu2/cpufreq/cpuinfo_max_freq",
        "3600000");
    mock_sysfs_create_file("sys/devices/system/cpu/cpu2/online", "1");

    /* Storage sdz with queue subtree (minimal: scheduler + read_ahead). */
    mock_sysfs_create_dir("sys/block/sdz/queue");
    mock_sysfs_create_dir("sys/block/sdz/device");
    mock_sysfs_create_file("sys/block/sdz/queue/scheduler",
                           "mq-deadline kyber [bfq] none");
    mock_sysfs_create_file("sys/block/sdz/queue/read_ahead_kb", "128");

    /* NIC eth9. */
    mock_sysfs_create_dir("sys/class/net/eth9");
    mock_sysfs_create_file("sys/class/net/eth9/mtu", "1500");

    /* PCI device 0000:07:00.0. */
    mock_sysfs_create_dir("sys/bus/pci/devices/0000:07:00.0/power");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:07:00.0/current_link_speed",
        "8.0 GT/s");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:07:00.0/current_link_width", "16");

    /* Thermal zone (reuse thermal_zone0 path; create idempotently). */
    mock_sysfs_create_dir("sys/class/thermal/thermal_zone0");
    mock_sysfs_create_file("sys/class/thermal/thermal_zone0/temp",
                           "45000");
    mock_sysfs_create_file("sys/class/thermal/thermal_zone0/policy",
                           "step_wise");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/available_policies",
        "step_wise fair-share");

    /* USB device 3-1 with power subtree. */
    mock_sysfs_create_dir("sys/bus/usb/devices/3-1/power");
    mock_sysfs_create_file("sys/bus/usb/devices/3-1/power/control",
                           "auto");

    /* VM tunables for the mem setters. */
    mock_sysfs_create_file("proc/sys/vm/swappiness", "60");
    mock_sysfs_create_file("proc/sys/vm/nr_hugepages", "1024");
    mock_sysfs_create_file("proc/sys/vm/overcommit_memory", "0");
    mock_sysfs_create_file(
        "sys/kernel/mm/transparent_hugepage/enabled",
        "[always] madvise never");
}

/* ── 1. NULL argument to zenctl_*_open() ─────────────────────────── */

static void test_open_null_args(void)
{
    zenctl_err_t err;

    /* String-arg opens: NULL name -> EINVAL. */
    memset(&err, 0, sizeof(err));
    OK(zenctl_storage_open(NULL, &err) == NULL,
       "storage_open(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "storage_open(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_net_open(NULL, &err) == NULL,
       "net_open(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "net_open(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_open(NULL, &err) == NULL,
       "pcie_open(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "pcie_open(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_open(NULL, &err) == NULL,
       "usb_open(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "usb_open(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_bt_open(NULL, &err) == NULL,
       "bt_open(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "bt_open(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_open(NULL, &err) == NULL,
       "wireless_open(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "wireless_open(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_open(NULL, &err) == NULL,
       "thermal_open(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "thermal_open(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_open_path(NULL, &err) == NULL,
       "gpu_open_path(NULL) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "gpu_open_path(NULL) sets ZENCTL_ERR_EINVAL");

    /* Int-arg opens: NULL err must not crash. The function still
     * validates its int arg, so a bad index returns NULL cleanly. */
    OK(zenctl_cpu_open(-1, NULL) == NULL,
       "cpu_open(-1, NULL) returns NULL without crashing");
    OK(zenctl_gpu_open(-1, NULL) == NULL,
       "gpu_open(-1, NULL) returns NULL without crashing");
}

/* ── 2. Non-existent path/name to zenctl_*_open() ────────────────── */

static void test_open_missing_path(void)
{
    zenctl_err_t err;

    memset(&err, 0, sizeof(err));
    OK(zenctl_cpu_open(99999, &err) == NULL,
       "cpu_open(99999) returns NULL (no such CPU)");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "cpu_open(99999) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_storage_open("nosuchdev", &err) == NULL,
       "storage_open(\"nosuchdev\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "storage_open(missing) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_net_open("nosuchiface", &err) == NULL,
       "net_open(\"nosuchiface\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "net_open(missing) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_open("0000:99:99.9", &err) == NULL,
       "pcie_open(\"0000:99:99.9\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "pcie_open(missing) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_open("9-9", &err) == NULL,
       "usb_open(\"9-9\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "usb_open(missing) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_bt_open("hci9", &err) == NULL,
       "bt_open(\"hci9\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "bt_open(missing) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_open("phy9", &err) == NULL,
       "wireless_open(\"phy9\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "wireless_open(missing) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_open("thermal_zone9", &err) == NULL,
       "thermal_open(\"thermal_zone9\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "thermal_open(missing) sets ZENCTL_ERR_ENOENT");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_open(99, &err) == NULL,
       "gpu_open(99) returns NULL (no such card)");
    /* gpu_open reads <sysfs>/device/vendor; missing file -> ENOENT. */
    OK(err.code == ZENCTL_ERR_ENOENT,
       "gpu_open(missing) sets ZENCTL_ERR_ENOENT");
}

/* ── 3. NULL out parameter to zenctl_*_get_*() ───────────────────── */

static void test_get_null_out(void)
{
    zenctl_err_t err;
    zenctl_cpu_t *cpu = zenctl_cpu_open(2, &err);
    zenctl_storage_t *st = zenctl_storage_open("sdz", &err);
    zenctl_net_t *net = zenctl_net_open("eth9", &err);
    zenctl_pcie_t *pcie = zenctl_pcie_open("0000:07:00.0", &err);
    zenctl_thermal_t *tz = zenctl_thermal_open("thermal_zone0", &err);

    OK(cpu && st && net && pcie && tz,
       "all handles open against fixtures for NULL-out tests");

    if (cpu) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_cpu_get_governor(cpu, NULL, &err) == -1,
           "cpu_get_governor(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "cpu_get_governor(NULL out) sets ZENCTL_ERR_EINVAL");

        memset(&err, 0, sizeof(err));
        OK(zenctl_cpu_get_freq_max(cpu, NULL, &err) == -1,
           "cpu_get_freq_max(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "cpu_get_freq_max(NULL out) sets ZENCTL_ERR_EINVAL");

        memset(&err, 0, sizeof(err));
        OK(zenctl_cpu_get_online(cpu, NULL, &err) == -1,
           "cpu_get_online(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "cpu_get_online(NULL out) sets ZENCTL_ERR_EINVAL");
    }

    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_get_swappiness(NULL, &err) == -1,
       "mem_get_swappiness(NULL out) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "mem_get_swappiness(NULL out) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_get_nr_hugepages(NULL, &err) == -1,
       "mem_get_nr_hugepages(NULL out) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "mem_get_nr_hugepages(NULL out) sets ZENCTL_ERR_EINVAL");

    if (st) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_storage_get_scheduler(st, NULL, &err) == -1,
           "storage_get_scheduler(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "storage_get_scheduler(NULL out) sets ZENCTL_ERR_EINVAL");

        memset(&err, 0, sizeof(err));
        OK(zenctl_storage_get_read_ahead(st, NULL, &err) == -1,
           "storage_get_read_ahead(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "storage_get_read_ahead(NULL out) sets ZENCTL_ERR_EINVAL");
    }

    if (net) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_net_get_mtu(net, NULL, &err) == -1,
           "net_get_mtu(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "net_get_mtu(NULL out) sets ZENCTL_ERR_EINVAL");
    }

    if (pcie) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_pcie_get_link_width(pcie, NULL, &err) == -1,
           "pcie_get_link_width(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "pcie_get_link_width(NULL out) sets ZENCTL_ERR_EINVAL");
    }

    if (tz) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_thermal_get_temp(tz, NULL, &err) == -1,
           "thermal_get_temp(NULL out) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "thermal_get_temp(NULL out) sets ZENCTL_ERR_EINVAL");
    }

    if (cpu) zenctl_cpu_close(cpu);
    if (st) zenctl_storage_close(st);
    if (net) zenctl_net_close(net);
    if (pcie) zenctl_pcie_close(pcie);
    if (tz) zenctl_thermal_close(tz);
}

/* ── 4. NULL handle to zenctl_*_get_*() ──────────────────────────── */

static void test_get_null_handle(void)
{
    zenctl_err_t err;
    char *out = NULL;
    int64_t i64 = 0;
    int iv = 0;
    bool bv = false;

    memset(&err, 0, sizeof(err));
    OK(zenctl_cpu_get_governor(NULL, &out, &err) == -1,
       "cpu_get_governor(NULL cpu) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "cpu_get_governor(NULL cpu) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_cpu_get_freq_max(NULL, &i64, &err) == -1,
       "cpu_get_freq_max(NULL cpu) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "cpu_get_freq_max(NULL cpu) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_storage_get_scheduler(NULL, &out, &err) == -1,
       "storage_get_scheduler(NULL st) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "storage_get_scheduler(NULL st) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_net_get_mtu(NULL, &iv, &err) == -1,
       "net_get_mtu(NULL net) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "net_get_mtu(NULL net) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_get_link_width(NULL, &iv, &err) == -1,
       "pcie_get_link_width(NULL pcie) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "pcie_get_link_width(NULL pcie) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_temp(NULL, &i64, &err) == -1,
       "thermal_get_temp(NULL tz) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "thermal_get_temp(NULL tz) sets ZENCTL_ERR_EINVAL");
    (void)bv;
}

/* ── 5. Out-of-range value to zenctl_*_set_*() ───────────────────── */

static void test_set_out_of_range(void)
{
    zenctl_err_t err;
    zenctl_cpu_t *cpu = zenctl_cpu_open(2, &err);
    zenctl_storage_t *st = zenctl_storage_open("sdz", &err);
    zenctl_net_t *net = zenctl_net_open("eth9", &err);

    OK(cpu && st && net, "handles open for out-of-range tests");

    if (cpu) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_cpu_set_freq_max(cpu, -1, &err) == -1,
           "cpu_set_freq_max(-1) returns -1 (negative)");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "cpu_set_freq_max(-1) sets ZENCTL_ERR_EINVAL");

        /* 100 THz is well above the 3.6 GHz cpuinfo_max_freq fixture. */
        memset(&err, 0, sizeof(err));
        OK(zenctl_cpu_set_freq_max(cpu, 100000000000000LL, &err) == -1,
           "cpu_set_freq_max(100 THz) returns -1 (above hw max)");
        OK(err.code == ZENCTL_ERR_ERANGE,
           "cpu_set_freq_max(above hw max) sets ZENCTL_ERR_ERANGE");
    }

    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_set_swappiness(-1, &err) == -1,
       "mem_set_swappiness(-1) returns -1");
    OK(err.code == ZENCTL_ERR_ERANGE,
       "mem_set_swappiness(-1) sets ZENCTL_ERR_ERANGE");

    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_set_swappiness(201, &err) == -1,
       "mem_set_swappiness(201) returns -1");
    OK(err.code == ZENCTL_ERR_ERANGE,
       "mem_set_swappiness(201) sets ZENCTL_ERR_ERANGE");

    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_set_overcommit(3, &err) == -1,
       "mem_set_overcommit(3) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "mem_set_overcommit(3) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_set_nr_hugepages(-1, &err) == -1,
       "mem_set_nr_hugepages(-1) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "mem_set_nr_hugepages(-1) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_set_thp((zenctl_thp_mode_t)99, &err) == -1,
       "mem_set_thp(99) returns -1 (invalid enum)");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "mem_set_thp(99) sets ZENCTL_ERR_EINVAL");

    if (st) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_storage_set_queue_depth(st, 0, &err) == -1,
           "storage_set_queue_depth(0) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "storage_set_queue_depth(0) sets ZENCTL_ERR_EINVAL");

        memset(&err, 0, sizeof(err));
        OK(zenctl_storage_set_read_ahead(st, -1, &err) == -1,
           "storage_set_read_ahead(-1) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "storage_set_read_ahead(-1) sets ZENCTL_ERR_EINVAL");
    }

    if (net) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_net_set_mtu(net, -1, &err) == -1,
           "net_set_mtu(-1) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "net_set_mtu(-1) sets ZENCTL_ERR_EINVAL");
    }

    memset(&err, 0, sizeof(err));
    OK(zenctl_power_suspend((zenctl_sleep_state_t)999, &err) == -1,
       "power_suspend(999) returns -1 (invalid enum)");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "power_suspend(999) sets ZENCTL_ERR_EINVAL");

    if (cpu) zenctl_cpu_close(cpu);
    if (st) zenctl_storage_close(st);
    if (net) zenctl_net_close(net);
}

/* ── 6. NULL value to zenctl_*_set_*() ───────────────────────────── */

static void test_set_null_value(void)
{
    zenctl_err_t err;
    zenctl_cpu_t *cpu = zenctl_cpu_open(2, &err);
    zenctl_storage_t *st = zenctl_storage_open("sdz", &err);
    zenctl_pcie_t *pcie = zenctl_pcie_open("0000:07:00.0", &err);
    zenctl_thermal_t *tz = zenctl_thermal_open("thermal_zone0", &err);
    zenctl_usb_t *usb = zenctl_usb_open("3-1", &err);

    OK(cpu && st && pcie && tz && usb,
       "handles open for NULL-value tests");

    if (cpu) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_cpu_set_governor(cpu, NULL, &err) == -1,
           "cpu_set_governor(NULL) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "cpu_set_governor(NULL) sets ZENCTL_ERR_EINVAL");
    }

    if (st) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_storage_set_scheduler(st, NULL, &err) == -1,
           "storage_set_scheduler(NULL) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "storage_set_scheduler(NULL) sets ZENCTL_ERR_EINVAL");

        memset(&err, 0, sizeof(err));
        OK(zenctl_storage_set_power_control(st, NULL, &err) == -1,
           "storage_set_power_control(NULL) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "storage_set_power_control(NULL) sets ZENCTL_ERR_EINVAL");
    }

    if (pcie) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_pcie_set_power_control(pcie, NULL, &err) == -1,
           "pcie_set_power_control(NULL) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "pcie_set_power_control(NULL) sets ZENCTL_ERR_EINVAL");
    }

    if (usb) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_usb_set_power_control(usb, NULL, &err) == -1,
           "usb_set_power_control(NULL) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "usb_set_power_control(NULL) sets ZENCTL_ERR_EINVAL");
    }

    if (tz) {
        memset(&err, 0, sizeof(err));
        OK(zenctl_thermal_set_policy(tz, NULL, &err) == -1,
           "thermal_set_policy(NULL) returns -1");
        OK(err.code == ZENCTL_ERR_EINVAL,
           "thermal_set_policy(NULL) sets ZENCTL_ERR_EINVAL");
    }

    if (cpu) zenctl_cpu_close(cpu);
    if (st) zenctl_storage_close(st);
    if (pcie) zenctl_pcie_close(pcie);
    if (tz) zenctl_thermal_close(tz);
    if (usb) zenctl_usb_close(usb);
}

/* ── 7. Permission boundary (mock): write to a read-only file ────── */
/*
 * chmod the swappiness fixture to 0444 and try to write. As non-root
 * the write fails with EACCES, which the library maps to
 * ZENCTL_ERR_EPERM. Root bypasses DAC so the test is skipped (counted
 * as ok with a note).
 */
static void test_permission_boundary(void)
{
    const char *rel = "proc/sys/vm/swappiness";
    char full[4096];
    const char *pfx = mock_sysfs_prefix();
    snprintf(full, sizeof(full), "%s/%s", pfx ? pfx : "", rel);

    /* Make sure the fixture exists and is currently writable. */
    mock_sysfs_create_file(rel, "60");
    if (chmod(full, 0444) != 0) {
        OK(0, "chmod fixture to 0444 succeeded");
        return;
    }

    if (geteuid() == 0) {
        printf("ok %d: skip write-to-readonly test (running as root, "
               "DAC bypassed)\n", ++test_count);
        test_pass++;
        /* restore permissions so subsequent suites can write */
        chmod(full, 0644);
        return;
    }

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_mem_set_swappiness(40, &err);
    OK(rc == -1, "mem_set_swappiness on read-only file returns -1");
    OK(err.code == ZENCTL_ERR_EPERM,
       "mem_set_swappiness on read-only file sets ZENCTL_ERR_EPERM");

    /* restore for any later suite */
    chmod(full, 0644);
}

/* ── 8. Generic key-value API with malformed keys ────────────────── */

static void test_kv_malformed_keys(void)
{
    zenctl_ctx_t *ctx = zenctl_ctx_new(NULL);
    OK(ctx != NULL, "ctx_new for kv malformed-key tests");

    zenctl_err_t err;
    zenctl_val_t out;
    memset(&out, 0, sizeof(out));

    /* zenctl_get: NULL / empty / too-long / no-dot keys -> EINVAL */
    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, NULL, &out, &err) == -1,
       "zenctl_get(NULL key) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_get(NULL key) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, "", &out, &err) == -1,
       "zenctl_get(\"\") returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_get(\"\") sets ZENCTL_ERR_EINVAL");

    /* very long key (no dot, >4096) */
    char longkey[5000];
    memset(longkey, 'a', sizeof(longkey) - 1);
    longkey[sizeof(longkey) - 1] = '\0';
    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, longkey, &out, &err) == -1,
       "zenctl_get(>4096-byte key) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_get(>4096-byte key) sets ZENCTL_ERR_EINVAL");

    /* key without a domain separator */
    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, "noseparator", &out, &err) == -1,
       "zenctl_get(\"noseparator\") returns -1 (no domain prefix)");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_get(no-dot key) sets ZENCTL_ERR_EINVAL");

    /* valid-and-unknown key -> ENOTSUP (not EINVAL) */
    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, "unknown.key", &out, &err) == -1,
       "zenctl_get(\"unknown.key\") returns -1");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "zenctl_get(valid-but-unknown key) sets ZENCTL_ERR_ENOTSUP");

    /* zenctl_set: NULL key, empty key, NULL value, no-dot key */
    memset(&err, 0, sizeof(err));
    OK(zenctl_set(ctx, NULL, "60", &err) == -1,
       "zenctl_set(NULL key) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_set(NULL key) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_set(ctx, "", "60", &err) == -1,
       "zenctl_set(\"\") returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_set(\"\") sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_set(ctx, "mem.swappiness", NULL, &err) == -1,
       "zenctl_set(NULL value) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_set(NULL value) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_set(ctx, "noseparator", "60", &err) == -1,
       "zenctl_set(\"noseparator\") returns -1 (no domain prefix)");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_set(no-dot key) sets ZENCTL_ERR_EINVAL");

    zenctl_ctx_free(ctx);
}

/* ── Suite entry ─────────────────────────────────────────────────── */

int test_errors_suite(void)
{
    SUITE_START("error paths");
    build_fixtures();
    test_open_null_args();
    test_open_missing_path();
    test_get_null_out();
    test_get_null_handle();
    test_set_out_of_range();
    test_set_null_value();
    test_permission_boundary();
    test_kv_malformed_keys();
    SUITE_END();
    return SUITE_FAILURES();
}
