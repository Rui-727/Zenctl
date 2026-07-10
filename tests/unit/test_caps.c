/* test_caps.c - capability query tests.
 *
 * zenctl_query_cap() maps a small set of well-known keys (mem.*,
 * cpu.<N>.*) to their backing sysfs/procfs path and reports one of:
 *   ZENCTL_CAP_UNAVAILABLE  file is missing
 *   ZENCTL_CAP_READONLY     file exists but is not writable
 *   ZENCTL_CAP_AVAILABLE    file exists and is writable
 *
 * The READONLY case only fires for non-root callers (root bypasses
 * DAC); the test is skipped with an "ok" when euid == 0.
 *
 * Also covers zenctl_version() and a couple of zenctl_strerror
 * spot-checks since those live alongside query_cap in core.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void build_caps_fixtures(void)
{
    /* mem.* paths that key_to_path() knows about. */
    mock_sysfs_create_file("proc/sys/vm/swappiness", "60");
    mock_sysfs_create_file("proc/sys/vm/nr_hugepages", "1024");
    mock_sysfs_create_file("proc/sys/vm/overcommit_memory", "0");
    mock_sysfs_create_file("proc/sys/vm/vfs_cache_pressure", "100");
    mock_sysfs_create_file(
        "sys/kernel/mm/transparent_hugepage/enabled",
        "[always] madvise never");

    /* cpu.7.* paths. Use a high index no other suite touches. */
    mock_sysfs_create_dir("sys/devices/system/cpu/cpu7/cpufreq");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu7/cpufreq/scaling_governor",
        "performance");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu7/cpufreq/scaling_min_freq",
        "800000");
    mock_sysfs_create_file(
        "sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq",
        "3600000");
    mock_sysfs_create_file("sys/devices/system/cpu/cpu7/online", "1");
}

static void test_cap_available(void)
{
    zenctl_ctx_t *ctx = zenctl_ctx_new(NULL);
    OK(ctx != NULL, "ctx_new for cap-available tests");

    OK(zenctl_query_cap(ctx, "mem.swappiness") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"mem.swappiness\") -> AVAILABLE (fixture exists, writable)");
    OK(zenctl_query_cap(ctx, "mem.nr_hugepages") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"mem.nr_hugepages\") -> AVAILABLE");
    OK(zenctl_query_cap(ctx, "mem.overcommit") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"mem.overcommit\") -> AVAILABLE");
    OK(zenctl_query_cap(ctx, "mem.vfs_cache_pressure") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"mem.vfs_cache_pressure\") -> AVAILABLE");
    OK(zenctl_query_cap(ctx, "mem.thp") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"mem.thp\") -> AVAILABLE");

    OK(zenctl_query_cap(ctx, "cpu.7.governor") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"cpu.7.governor\") -> AVAILABLE");
    OK(zenctl_query_cap(ctx, "cpu.7.freq_min") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"cpu.7.freq_min\") -> AVAILABLE");
    OK(zenctl_query_cap(ctx, "cpu.7.freq_max") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"cpu.7.freq_max\") -> AVAILABLE");
    OK(zenctl_query_cap(ctx, "cpu.7.online") == ZENCTL_CAP_AVAILABLE,
       "query_cap(\"cpu.7.online\") -> AVAILABLE");

    zenctl_ctx_free(ctx);
}

static void test_cap_unavailable(void)
{
    zenctl_ctx_t *ctx = zenctl_ctx_new(NULL);
    OK(ctx != NULL, "ctx_new for cap-unavailable tests");

    /* cpu.999 has no fixture -> UNAVAILABLE. */
    OK(zenctl_query_cap(ctx, "cpu.999.governor") == ZENCTL_CAP_UNAVAILABLE,
       "query_cap(\"cpu.999.governor\") -> UNAVAILABLE (no fixture)");

    /* Unknown domain prefix -> key_to_path returns NULL -> UNAVAILABLE. */
    OK(zenctl_query_cap(ctx, "unknown.key") == ZENCTL_CAP_UNAVAILABLE,
       "query_cap(\"unknown.key\") -> UNAVAILABLE");

    /* Unknown attribute on a known domain -> UNAVAILABLE. */
    OK(zenctl_query_cap(ctx, "cpu.7.bogus_attr") == ZENCTL_CAP_UNAVAILABLE,
       "query_cap(\"cpu.7.bogus_attr\") -> UNAVAILABLE (unknown attr)");

    /* NULL / empty key -> UNAVAILABLE (not a crash). */
    OK(zenctl_query_cap(ctx, NULL) == ZENCTL_CAP_UNAVAILABLE,
       "query_cap(NULL) -> UNAVAILABLE");
    OK(zenctl_query_cap(ctx, "") == ZENCTL_CAP_UNAVAILABLE,
       "query_cap(\"\") -> UNAVAILABLE");

    /* NULL ctx is tolerated (ctx is not yet used by the lookup). */
    OK(zenctl_query_cap(NULL, "mem.swappiness") == ZENCTL_CAP_AVAILABLE,
       "query_cap(NULL ctx, valid key) still works");

    zenctl_ctx_free(ctx);
}

static void test_cap_readonly(void)
{
    zenctl_ctx_t *ctx = zenctl_ctx_new(NULL);
    OK(ctx != NULL, "ctx_new for cap-readonly tests");

    /* chmod the swappiness fixture to 0444 and probe. As non-root the
     * probe must report READONLY. Root bypasses DAC so the test is
     * skipped (counted as ok with a note). */
    const char *rel = "proc/sys/vm/swappiness";
    char full[4096];
    const char *pfx = mock_sysfs_prefix();
    snprintf(full, sizeof(full), "%s/%s", pfx ? pfx : "", rel);
    mock_sysfs_create_file(rel, "60");
    if (chmod(full, 0444) != 0) {
        OK(0, "chmod fixture to 0444 succeeded (readonly test)");
        zenctl_ctx_free(ctx);
        return;
    }

    if (geteuid() == 0) {
        printf("ok %d: skip cap-readonly test (running as root, "
               "DAC bypassed)\n", ++test_count);
        test_pass++;
        chmod(full, 0644);
        zenctl_ctx_free(ctx);
        return;
    }

    OK(zenctl_query_cap(ctx, "mem.swappiness") == ZENCTL_CAP_READONLY,
       "query_cap(\"mem.swappiness\") -> READONLY when file is 0444");

    /* Restore writability for any later suite. */
    chmod(full, 0644);
    zenctl_ctx_free(ctx);
}

static void test_version_and_strerror(void)
{
    /* zenctl_version returns "MAJOR.MINOR.PATCH"; the Makefile injects
     * 0/1/0 by default so the string is "0.1.0". */
    const char *v = zenctl_version();
    OK(v != NULL, "zenctl_version returns non-NULL");
    OK(strstr(v, "0.1.0") != NULL || strstr(v, ".") != NULL,
       "zenctl_version returns a dotted version string");

    /* Calling twice returns the same pointer (static buffer). */
    OK(zenctl_version() == v, "zenctl_version is stable across calls");

    /* Spot-check strerror. The full table is exercised in
     * test_kv_api.c; here we just make sure the common codes map to
     * non-empty strings. */
    OK(zenctl_strerror(ZENCTL_OK) != NULL,
       "strerror(ZENCTL_OK) non-NULL");
    OK(zenctl_strerror(ZENCTL_ERR_EPERM) != NULL,
       "strerror(ZENCTL_ERR_EPERM) non-NULL");
    OK(zenctl_strerror(99999) != NULL,
       "strerror(unknown) non-NULL");
}

int test_caps_suite(void)
{
    SUITE_START("capability query");
    build_caps_fixtures();
    test_cap_available();
    test_cap_unavailable();
    test_cap_readonly();
    test_version_and_strerror();
    SUITE_END();
    return SUITE_FAILURES();
}
