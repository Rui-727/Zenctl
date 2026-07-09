/* test_cpu.c - CPU domain unit tests against a mock sysfs tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_open_success(void)
{
    /* Create the cpu0 directory and the cpufreq subtree. */
    mock_sysfs_create_dir("sys/devices/system/cpu/cpu0/cpufreq");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
        "performance");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors",
        "performance powersched");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
        "3600000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",
        "800000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq",
        "800000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
        "3600000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu0/online", "1");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_cpu_t *cpu = zenctl_cpu_open(0, &err);
    OK(cpu != NULL, "cpu_open(0) succeeds against mock tree");
    OK(err.code == ZENCTL_OK, "cpu_open(0) leaves err.code == ZENCTL_OK");
    if (!cpu) return;

    /* governor read */
    char *gov = NULL;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_cpu_get_governor(cpu, &gov, &err);
    OK(rc == 0, "get_governor returns 0");
    OK(gov && strcmp(gov, "performance") == 0,
       "get_governor returns \"performance\"");
    free(gov);

    /* governor write (validated against scaling_available_governors) */
    memset(&err, 0, sizeof(err));
    rc = zenctl_cpu_set_governor(cpu, "performance", &err);
    OK(rc == 0, "set_governor(performance) returns 0");
    char buf[64];
    int n = mock_sysfs_read_file(
        "sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
        buf, sizeof(buf));
    OK(n >= 0, "scaling_governor file exists after write");
    OK(strcmp(buf, "performance") == 0,
       "scaling_governor file contains \"performance\"");

    /* governor write rejects unknown name */
    memset(&err, 0, sizeof(err));
    rc = zenctl_cpu_set_governor(cpu, "bogus", &err);
    OK(rc == -1, "set_governor(bogus) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_governor(bogus) sets ZENCTL_ERR_EINVAL");

    /* freq_max: kernel reports kHz, API reports Hz */
    int64_t hz = 0;
    memset(&err, 0, sizeof(err));
    rc = zenctl_cpu_get_freq_max(cpu, &hz, &err);
    OK(rc == 0, "get_freq_max returns 0");
    OK(hz == 3600000000LL, "get_freq_max converts 3600000 kHz -> 3.6 GHz");

    /* online read */
    bool online = false;
    memset(&err, 0, sizeof(err));
    rc = zenctl_cpu_get_online(cpu, &online, &err);
    OK(rc == 0, "get_online returns 0");
    OK(online == true, "get_online returns true for \"1\"");

    /* online write */
    memset(&err, 0, sizeof(err));
    rc = zenctl_cpu_set_online(cpu, false, &err);
    OK(rc == 0, "set_online(false) returns 0");
    n = mock_sysfs_read_file(
        "sys/devices/system/cpu/cpu0/online", buf, sizeof(buf));
    OK(n >= 0, "online file exists after write");
    OK(strcmp(buf, "0") == 0, "online file contains \"0\" after set_online(false)");

    zenctl_cpu_close(cpu);
}

static void test_open_missing_cpu(void)
{
    /* cpu99999 has no fixture entry; access() must fail with ENOENT. */
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_cpu_t *cpu = zenctl_cpu_open(99999, &err);
    OK(cpu == NULL, "cpu_open(99999) returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "cpu_open(99999) sets ZENCTL_ERR_ENOENT");
}

static void test_open_bad_index(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_cpu_t *cpu = zenctl_cpu_open(-1, &err);
    OK(cpu == NULL, "cpu_open(-1) returns NULL");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "cpu_open(-1) sets ZENCTL_ERR_EINVAL");
}

static void test_missing_cpufreq_capability(void)
{
    /* Create cpu1 with an `online` file but no cpufreq subtree.
     * Reads of files under cpufreq must fail cleanly. The library
     * currently surfaces this as ZENCTL_ERR_ENOENT (the underlying
     * file is missing); ENOTSUP would arguably be more semantically
     * correct but is not what cpu.c returns today. */
    mock_sysfs_create_dir("sys/devices/system/cpu/cpu1");
    mock_sysfs_create_file("sys/devices/system/cpu/cpu1/online", "1");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_cpu_t *cpu = zenctl_cpu_open(1, &err);
    OK(cpu != NULL, "cpu_open(1) succeeds with no cpufreq dir");
    if (!cpu) return;

    char *gov = NULL;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_cpu_get_governor(cpu, &gov, &err);
    OK(rc == -1, "get_governor fails when cpufreq is absent");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "get_governor sets ZENCTL_ERR_ENOENT (missing cpufreq -> ENOENT)");

    int64_t hz = 0;
    memset(&err, 0, sizeof(err));
    rc = zenctl_cpu_get_freq_max(cpu, &hz, &err);
    OK(rc == -1, "get_freq_max fails when cpufreq is absent");

    zenctl_cpu_close(cpu);
}

int test_cpu_suite(void)
{
    SUITE_START("CPU domain");
    test_open_success();
    test_open_missing_cpu();
    test_open_bad_index();
    test_missing_cpufreq_capability();
    SUITE_END();
    return SUITE_FAILURES();
}
