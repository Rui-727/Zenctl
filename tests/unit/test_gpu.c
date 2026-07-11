/* test_gpu.c - GPU domain unit tests against a mock sysfs tree.
 *
 * Covers the i915 RPS / RC6 paths that were fixed in the audit:
 *   - Modern kernel (6.2+) layout: device/gt/gt0/<name>_mhz
 *   - Legacy kernel layout: device/<name> (no _mhz suffix)
 *   - rc6_enable bitmask: get returns bit 0 (RC6) as a bool
 *   - rc6_enable read-only on modern kernels: set returns ENOTSUP
 *
 * The mock fixture creates a fake /sys/class/drm/card0/device tree
 * with a driver symlink pointing at "../../../bus/pci/drivers/i915"
 * so gpu_open_path() picks up driver = "i915".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

/* Build a minimal i915 cardN mock: vendor file + driver symlink.
 * Each test uses a distinct card index so the mock fixture state
 * from one test doesn't leak into the next (the mock tree is shared
 * across the whole suite). Returns a heap-allocated gpu handle
 * (caller closes) or NULL. */
static zenctl_gpu_t *open_mock_i915(int card_index, zenctl_err_t *err)
{
    char path[256];
    snprintf(path, sizeof(path),
             "sys/class/drm/card%d/device", card_index);
    mock_sysfs_create_dir(path);
    snprintf(path, sizeof(path),
             "sys/class/drm/card%d/device/vendor", card_index);
    mock_sysfs_create_file(path, "0x8086");
    /* The library readlink()s .../device/driver and takes the basename
     * of the target. Point it at a path ending in "i915". */
    snprintf(path, sizeof(path),
             "sys/class/drm/card%d/device/driver", card_index);
    mock_sysfs_create_symlink(path, "../../../bus/pci/drivers/i915");
    snprintf(path, sizeof(path),
             "sys/class/drm/card%d", card_index);
    /* gpu_open_path stores the sysfs path it's given; pass the bare
     * /sys/class/drm/cardN form and let the mock redirect it. */
    char sysfs_path[128];
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/drm/card%d", card_index);
    return zenctl_gpu_open_path(sysfs_path, err);
}

/* Make a file read-only via chmod(). The library's set_rc6 calls
 * access(path, W_OK) to detect the read-only case; chmod 0444 makes
 * W_OK fail with EACCES for non-root (test runs as non-root). */
static void make_readonly(const char *rel_path)
{
    char full[4096];
    const char *prefix = mock_sysfs_prefix();
    snprintf(full, sizeof(full), "%s/%s", prefix ? prefix : "", rel_path);
    chmod(full, 0444);
}

static void test_i915_rps_modern_path(void)
{
    /* Modern kernels put RPS files under device/gt/gt0/<name>_mhz. */
    const int card = 0;
    mock_sysfs_create_dir("sys/class/drm/card0/device/gt");
    mock_sysfs_create_dir("sys/class/drm/card0/device/gt/gt0");
    mock_sysfs_create_file(
        "sys/class/drm/card0/device/gt/gt0/rps_min_freq_mhz", "300");
    mock_sysfs_create_file(
        "sys/class/drm/card0/device/gt/gt0/rps_max_freq_mhz", "1500");
    mock_sysfs_create_file(
        "sys/class/drm/card0/device/gt/gt0/rps_cur_freq_mhz", "450");
    mock_sysfs_create_file(
        "sys/class/drm/card0/device/gt/gt0/rps_boost_freq_mhz", "1200");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *gpu = open_mock_i915(card, &err);
    OK(gpu != NULL, "gpu_open_path(/sys/class/drm/card0) succeeds on mock");
    if (!gpu) return;

    int v = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_min(gpu, &v, &err) == 0,
       "i915_get_rps_min returns 0 (modern path)");
    OK(v == 300, "i915_get_rps_min returns 300 from gt/gt0/rps_min_freq_mhz");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_max(gpu, &v, &err) == 0,
       "i915_get_rps_max returns 0 (modern path)");
    OK(v == 1500, "i915_get_rps_max returns 1500");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_cur(gpu, &v, &err) == 0,
       "i915_get_rps_cur returns 0 (modern path)");
    OK(v == 450, "i915_get_rps_cur returns 450");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_boost(gpu, &v, &err) == 0,
       "i915_get_rps_boost returns 0 (modern path)");
    OK(v == 1200, "i915_get_rps_boost returns 1200");

    /* Writes go to the same modern path. */
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_set_rps_min(gpu, 350, &err) == 0,
       "i915_set_rps_min(350) returns 0");
    char buf[64];
    int n = mock_sysfs_read_file(
        "sys/class/drm/card0/device/gt/gt0/rps_min_freq_mhz",
        buf, sizeof(buf));
    OK(n >= 0, "rps_min_freq_mhz file exists after set_rps_min");
    OK(strcmp(buf, "350") == 0,
       "set_rps_min wrote 350 to gt/gt0/rps_min_freq_mhz");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_set_rps_max(gpu, 1400, &err) == 0,
       "i915_set_rps_max(1400) returns 0");
    n = mock_sysfs_read_file(
        "sys/class/drm/card0/device/gt/gt0/rps_max_freq_mhz",
        buf, sizeof(buf));
    OK(strcmp(buf, "1400") == 0,
       "set_rps_max wrote 1400 to gt/gt0/rps_max_freq_mhz");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_set_rps_boost(gpu, 1100, &err) == 0,
       "i915_set_rps_boost(1100) returns 0");
    n = mock_sysfs_read_file(
        "sys/class/drm/card0/device/gt/gt0/rps_boost_freq_mhz",
        buf, sizeof(buf));
    OK(strcmp(buf, "1100") == 0,
       "set_rps_boost wrote 1100 to gt/gt0/rps_boost_freq_mhz");

    /* Negative frequency -> ERANGE. */
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_set_rps_min(gpu, -1, &err) == -1,
       "i915_set_rps_min(-1) rejected");
    OK(err.code == ZENCTL_ERR_ERANGE,
       "i915_set_rps_min(-1) sets ZENCTL_ERR_ERANGE");

    zenctl_gpu_close(gpu);
}

static void test_i915_rps_legacy_path(void)
{
    /* Older kernels put RPS files directly under device/<name> (no
     * _mhz suffix, no gt/gt0/ subdir). The library should fall back
     * to this path when the modern files are absent. */
    const int card = 1;
    mock_sysfs_create_file("sys/class/drm/card1/device/rps_min_freq", "300");
    mock_sysfs_create_file("sys/class/drm/card1/device/rps_max_freq", "1500");
    mock_sysfs_create_file("sys/class/drm/card1/device/rps_cur_freq", "450");
    mock_sysfs_create_file("sys/class/drm/card1/device/rps_boost_freq",
                           "1200");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *gpu = open_mock_i915(card, &err);
    OK(gpu != NULL, "gpu_open_path succeeds on legacy i915 mock");
    if (!gpu) return;

    int v = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_min(gpu, &v, &err) == 0,
       "i915_get_rps_min returns 0 (legacy fallback)");
    OK(v == 300, "i915_get_rps_min returns 300 from device/rps_min_freq");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_max(gpu, &v, &err) == 0,
       "i915_get_rps_max returns 0 (legacy fallback)");
    OK(v == 1500, "i915_get_rps_max returns 1500");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_cur(gpu, &v, &err) == 0,
       "i915_get_rps_cur returns 0 (legacy fallback)");
    OK(v == 450, "i915_get_rps_cur returns 450");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_boost(gpu, &v, &err) == 0,
       "i915_get_rps_boost returns 0 (legacy fallback)");
    OK(v == 1200, "i915_get_rps_boost returns 1200");

    /* Writes go to the legacy path. */
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_set_rps_min(gpu, 350, &err) == 0,
       "i915_set_rps_min(350) returns 0 (legacy fallback)");
    char buf[64];
    mock_sysfs_read_file("sys/class/drm/card1/device/rps_min_freq",
                         buf, sizeof(buf));
    OK(strcmp(buf, "350") == 0,
       "set_rps_min wrote 350 to legacy device/rps_min_freq");

    zenctl_gpu_close(gpu);
}

static void test_i915_rc6_bitmask(void)
{
    /* rc6_enable is a bitmask: bit 0 = RC6, bit 1 = RC6p, bit 2 = RC6pp.
     * The library exposes bit 0 only. */
    const int card = 2;
    mock_sysfs_create_dir("sys/class/drm/card2/device/gt");
    mock_sysfs_create_dir("sys/class/drm/card2/device/gt/gt0");
    /* 0x5 = RC6 + RC6pp -> bit 0 set, get_rc6 returns true */
    mock_sysfs_create_file("sys/class/drm/card2/device/gt/gt0/rc6_enable",
                           "5");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *gpu = open_mock_i915(card, &err);
    OK(gpu != NULL, "gpu_open_path succeeds for rc6 test");
    if (!gpu) return;

    bool on = false;
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rc6(gpu, &on, &err) == 0,
       "i915_get_rc6 returns 0");
    OK(on == true, "i915_get_rc6 returns true when bit 0 is set (value=5)");

    /* Flip the value to 0x2 (RC6p only, no RC6) and re-read. */
    mock_sysfs_create_file("sys/class/drm/card2/device/gt/gt0/rc6_enable",
                           "2");
    memset(&err, 0, sizeof(err));
    on = true;
    OK(zenctl_gpu_i915_get_rc6(gpu, &on, &err) == 0,
       "i915_get_rc6 returns 0 (second call)");
    OK(on == false,
       "i915_get_rc6 returns false when bit 0 is clear (value=2)");

    /* rc6_enable = 0 */
    mock_sysfs_create_file("sys/class/drm/card2/device/gt/gt0/rc6_enable",
                           "0");
    memset(&err, 0, sizeof(err));
    on = true;
    OK(zenctl_gpu_i915_get_rc6(gpu, &on, &err) == 0,
       "i915_get_rc6 returns 0 (value=0)");
    OK(on == false, "i915_get_rc6 returns false when value=0");

    zenctl_gpu_close(gpu);
}

static void test_i915_rc6_set_writable(void)
{
    /* When rc6_enable is writable (older kernels), set_rc6 writes
     * "1" or "0" to the file. The mock fixture is writable by default. */
    const int card = 3;
    mock_sysfs_create_dir("sys/class/drm/card3/device/gt");
    mock_sysfs_create_dir("sys/class/drm/card3/device/gt/gt0");
    mock_sysfs_create_file("sys/class/drm/card3/device/gt/gt0/rc6_enable",
                           "0");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *gpu = open_mock_i915(card, &err);
    OK(gpu != NULL, "gpu_open_path succeeds for rc6 writable test");
    if (!gpu) return;

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_set_rc6(gpu, true, &err) == 0,
       "i915_set_rc6(true) returns 0 on writable file");
    char buf[64];
    mock_sysfs_read_file("sys/class/drm/card3/device/gt/gt0/rc6_enable",
                         buf, sizeof(buf));
    OK(strcmp(buf, "1") == 0,
       "set_rc6(true) wrote \"1\" to rc6_enable");

    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_set_rc6(gpu, false, &err) == 0,
       "i915_set_rc6(false) returns 0 on writable file");
    mock_sysfs_read_file("sys/class/drm/card3/device/gt/gt0/rc6_enable",
                         buf, sizeof(buf));
    OK(strcmp(buf, "0") == 0,
       "set_rc6(false) wrote \"0\" to rc6_enable");

    zenctl_gpu_close(gpu);
}

static void test_i915_rc6_set_readonly(void)
{
    /* Modern kernels expose rc6_enable as 0444. The library detects
     * the read-only case via access(path, W_OK) and returns
     * ZENCTL_ERR_ENOTSUP with "RC6 is read-only on this kernel". */
    const int card = 4;
    mock_sysfs_create_dir("sys/class/drm/card4/device/gt");
    mock_sysfs_create_dir("sys/class/drm/card4/device/gt/gt0");
    mock_sysfs_create_file("sys/class/drm/card4/device/gt/gt0/rc6_enable",
                           "1");
    make_readonly("sys/class/drm/card4/device/gt/gt0/rc6_enable");

    /* If the test is running as root, chmod 0444 won't block writes
     * (root bypasses DAC). Skip the assertion in that case. */
    bool root = (geteuid() == 0);

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *gpu = open_mock_i915(card, &err);
    OK(gpu != NULL, "gpu_open_path succeeds for rc6 read-only test");
    if (!gpu) return;

    memset(&err, 0, sizeof(err));
    int rc = zenctl_gpu_i915_set_rc6(gpu, true, &err);
    if (root) {
        OK(rc == 0, "i915_set_rc6(true) returns 0 (root bypasses 0444)");
    } else {
        OK(rc == -1, "i915_set_rc6(true) rejected on read-only file");
        OK(err.code == ZENCTL_ERR_ENOTSUP,
           "i915_set_rc6(read-only) sets ZENCTL_ERR_ENOTSUP");
        OK(strstr(err.message, "read-only") != NULL,
           "i915_set_rc6(read-only) message mentions 'read-only'");
    }

    zenctl_gpu_close(gpu);
}

static void test_i915_missing_attr(void)
{
    /* When neither the modern nor legacy path exists, get/set should
     * return ENOENT. Use a fresh card index so no gt/gt0/ files
     * exist from earlier tests. */
    const int card = 5;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *gpu = open_mock_i915(card, &err);
    OK(gpu != NULL, "gpu_open_path succeeds for missing-attr test");
    if (!gpu) return;

    int v = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_min(gpu, &v, &err) == -1,
       "i915_get_rps_min returns -1 when no path exists");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "i915_get_rps_min(missing) sets ZENCTL_ERR_ENOENT");

    bool on = false;
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rc6(gpu, &on, &err) == -1,
       "i915_get_rc6 returns -1 when no path exists");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "i915_get_rc6(missing) sets ZENCTL_ERR_ENOENT");

    zenctl_gpu_close(gpu);
}

static void test_i915_non_i915_driver_rejected(void)
{
    /* i915 entry points must reject GPUs whose driver is not i915. */
    const int card = 6;
    char path[256];
    snprintf(path, sizeof(path),
             "sys/class/drm/card%d/device", card);
    mock_sysfs_create_dir(path);
    snprintf(path, sizeof(path),
             "sys/class/drm/card%d/device/vendor", card);
    mock_sysfs_create_file(path, "0x1002");
    snprintf(path, sizeof(path),
             "sys/class/drm/card%d/device/driver", card);
    mock_sysfs_create_symlink(path, "../../../bus/pci/drivers/amdgpu");
    snprintf(path, sizeof(path),
             "/sys/class/drm/card%d", card);
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *gpu = zenctl_gpu_open_path(path, &err);
    OK(gpu != NULL, "gpu_open_path succeeds for amdgpu mock");
    if (!gpu) return;

    int v = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_gpu_i915_get_rps_min(gpu, &v, &err) == -1,
       "i915_get_rps_min rejected on amdgpu card");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "i915_get_rps_min(amdgpu) sets ZENCTL_ERR_ENOTSUP");

    zenctl_gpu_close(gpu);
}

int test_gpu_suite(void)
{
    SUITE_START("GPU domain (i915)");
    test_i915_rps_modern_path();
    test_i915_rps_legacy_path();
    test_i915_rc6_bitmask();
    test_i915_rc6_set_writable();
    test_i915_rc6_set_readonly();
    test_i915_missing_attr();
    test_i915_non_i915_driver_rejected();
    SUITE_END();
    return SUITE_FAILURES();
}
