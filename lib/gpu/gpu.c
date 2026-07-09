/* gpu.c - GPU domain implementation
 *
 * Backed by DRM sysfs and hwmon. v1 covers the common surface plus
 * AMDGPU-specific controls. NVML (NVIDIA) and i915-specific paths are
 * left as TODOs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>

#include "zenctl/internal.h"
#include "zenctl/gpu.h"


/* Path construction uses snprintf with PATH_MAX-sized buffers.
 * Real paths are <100 chars; theoretical truncation warnings
 * from -Wformat-truncation are noise here. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* ── local helpers ───────────────────────────────────────────────── */

/* Read a sysfs file into a malloced string with trailing whitespace
 * stripped. Returns NULL on error (err filled). */
static char *read_string_alloc(const char *path, zenctl_err_t *err)
{
    char buf[4096];
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return NULL;
    /* shared helper strips CR/LF; trim remaining trailing whitespace. */
    int n = (int)strlen(buf);
    while (n > 0 && isspace((unsigned char)buf[n - 1])) buf[--n] = '\0';
    char *s = strdup(buf);
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        "read_string_alloc");
        return NULL;
    }
    return s;
}

/* Read symlink and return the basename of the target (malloced). */
static char *readlink_basename(const char *link, zenctl_err_t *err)
{
    char buf[8192];
    ssize_t n = readlink(link, buf, sizeof(buf) - 1);
    if (n < 0) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), link);
        return NULL;
    }
    buf[n] = '\0';
    const char *slash = strrchr(buf, '/');
    const char *base = slash ? slash + 1 : buf;
    char *s = strdup(base);
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        "readlink_basename");
        return NULL;
    }
    return s;
}

/* Scan dir and return the first entry matching <prefix><digits>. */
static char *first_dir_with_prefix(const char *dir, const char *prefix,
                                   zenctl_err_t *err)
{
    DIR *d = opendir(dir);
    if (!d) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), dir);
        return NULL;
    }
    char *result = NULL;
    size_t plen = strlen(prefix);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, prefix, plen) != 0) continue;
        if (!isdigit((unsigned char)de->d_name[plen])) continue;
        result = strdup(de->d_name);
        if (!result)
            zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                            "first_dir_with_prefix");
        break;
    }
    closedir(d);
    if (!result && err && err->code == ZENCTL_OK)
        zenctl__set_err(err, ZENCTL_ERR_ENOENT, "no matching entry", dir);
    return result;
}

/* ── PCI vendor mapping ──────────────────────────────────────────── */

static const struct {
    uint32_t    id;
    const char *name;
} pci_vendors[] = {
    { 0x1002, "AMD"        },
    { 0x10de, "NVIDIA"     },
    { 0x8086, "Intel"      },
    { 0x1234, "Bochs"      },
    { 0x1af4, "Red Hat"    }, /* virtio */
    { 0x1b36, "Red Hat"    }, /* qxl */
    { 0x15ad, "VMware"     },
    { 0x1a03, "ASPEED"     },
    { 0x1013, "Cirrus"     },
    { 0x0033, "Parallels"  },
    { 0x1414, "Microsoft"  }, /* Hyper-V */
    { 0,      NULL         },
};

static const char *vendor_name_from_id(uint32_t id)
{
    for (int i = 0; pci_vendors[i].name; i++)
        if (pci_vendors[i].id == id) return pci_vendors[i].name;
    return "Unknown";
}

/* ── GPU object ──────────────────────────────────────────────────── */

struct zenctl_gpu {
    int   card_index;
    char *sysfs_path;     /* /sys/class/drm/cardN */
    char *device_path;    /* /sys/class/drm/cardN/device */
    char *driver;         /* amdgpu, i915, nouveau, xe, ... */
    char *vendor;         /* AMD, NVIDIA, Intel, ... */
    char *hwmon_path;     /* /sys/class/drm/cardN/device/hwmon/hwmonM */
};

/* Locate the first hwmonM subdir under <device_path>/hwmon/ and return
 * the full path (malloced). NULL if no hwmon is registered. */
static char *gpu_find_hwmon(const char *device_path, zenctl_err_t *err)
{
    char hwmon_dir[8192];
    snprintf(hwmon_dir, sizeof(hwmon_dir), "%s/hwmon", device_path);
    char *name = first_dir_with_prefix(hwmon_dir, "hwmon", err);
    if (!name) return NULL;
    char *out = malloc(8192);
    if (!out) {
        free(name);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "malloc failed",
                        "gpu_find_hwmon");
        return NULL;
    }
    snprintf(out, 8192, "%s/%s", hwmon_dir, name);
    free(name);
    return out;
}

/* Resolve /dev/dri/cardN to /sys/class/drm/cardN. Accepts both forms
 * as input. Returns malloced sysfs path or NULL. */
static char *resolve_sysfs_path(const char *drm_path, zenctl_err_t *err)
{
    if (!drm_path) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL drm_path",
                        "resolve_sysfs_path");
        return NULL;
    }
    const char *base = strrchr(drm_path, '/');
    base = base ? base + 1 : drm_path;
    char *out = malloc(8192);
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "malloc failed",
                        "resolve_sysfs_path");
        return NULL;
    }
    if (strncmp(drm_path, "/sys/class/drm/", 15) == 0)
        snprintf(out, 8192, "%s", drm_path);
    else
        snprintf(out, 8192, "/sys/class/drm/%s", base);
    return out;
}

zenctl_gpu_t *zenctl_gpu_open(int card_index, zenctl_err_t *err)
{
    if (card_index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "card index must be >= 0", "zenctl_gpu_open");
        return NULL;
    }
    char name[64];
    snprintf(name, sizeof(name), "card%d", card_index);
    char sysfs[8192];
    snprintf(sysfs, sizeof(sysfs), "/sys/class/drm/%s", name);
    return zenctl_gpu_open_path(sysfs, err);
}

zenctl_gpu_t *zenctl_gpu_open_path(const char *drm_path, zenctl_err_t *err)
{
    char *sysfs = resolve_sysfs_path(drm_path, err);
    if (!sysfs) return NULL;

    /* Sanity-check the device directory exists. */
    char device_path[8192];
    snprintf(device_path, sizeof(device_path), "%s/device", sysfs);
    char vendor_file[8192];
    snprintf(vendor_file, sizeof(vendor_file), "%s/vendor", device_path);
    char vf_buf[64];
    if (zenctl__read_file_string(vendor_file, vf_buf, sizeof(vf_buf), err) != 0) {
        free(sysfs);
        return NULL;
    }

    zenctl_gpu_t *gpu = calloc(1, sizeof(*gpu));
    if (!gpu) {
        free(sysfs);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed",
                        "zenctl_gpu_open_path");
        return NULL;
    }
    gpu->sysfs_path = sysfs;
    gpu->device_path = strdup(device_path);
    if (!gpu->device_path) {
        free(sysfs); free(gpu);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        "zenctl_gpu_open_path");
        return NULL;
    }

    /* Extract card index from name like "card0". */
    const char *base = strrchr(sysfs, '/');
    base = base ? base + 1 : sysfs;
    if (strncmp(base, "card", 4) == 0) gpu->card_index = atoi(base + 4);

    /* Driver: readlink on .../device/driver */
    char drv_link[8192];
    snprintf(drv_link, sizeof(drv_link), "%s/driver", device_path);
    gpu->driver = readlink_basename(drv_link, NULL);
    if (!gpu->driver)
        gpu->driver = strdup("unknown");  /* non-fatal */

    /* Vendor: parse hex PCI vendor id */
    errno = 0;
    char *end = NULL;
    unsigned long vid = strtoul(vf_buf, &end, 0);
    if (end != vf_buf)
        gpu->vendor = strdup(vendor_name_from_id((uint32_t)vid));
    else
        gpu->vendor = strdup("Unknown");

    /* Hwmon path is resolved lazily on first use. */
    return gpu;
}

void zenctl_gpu_close(zenctl_gpu_t *gpu)
{
    if (!gpu) return;
    free(gpu->sysfs_path);
    free(gpu->device_path);
    free(gpu->driver);
    free(gpu->vendor);
    free(gpu->hwmon_path);
    free(gpu);
}

/* Resolve and cache the hwmon path. Returns NULL on error. */
static const char *gpu_hwmon(zenctl_gpu_t *gpu, zenctl_err_t *err)
{
    if (gpu->hwmon_path) return gpu->hwmon_path;
    char *p = gpu_find_hwmon(gpu->device_path, err);
    if (!p) return NULL;
    gpu->hwmon_path = p;
    return p;
}

static int gpu_amdgpu_check(zenctl_gpu_t *gpu, zenctl_err_t *err)
{
    if (!gpu || !gpu->driver) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "gpu handle invalid",
                        "gpu_amdgpu_check");
        return -1;
    }
    if (strcmp(gpu->driver, "amdgpu") != 0) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx),
                 "amdgpu-specific op on driver=%s", gpu->driver);
        zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                        "operation only supported on amdgpu", ctx);
        return -1;
    }
    return 0;
}

/* ── Driver / vendor ─────────────────────────────────────────────── */

int zenctl_gpu_get_driver(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err)
{
    if (!gpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_get_driver");
        return -1;
    }
    char *s = strdup(gpu->driver ? gpu->driver : "unknown");
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        "zenctl_gpu_get_driver");
        return -1;
    }
    *out = s;
    return 0;
}

int zenctl_gpu_get_vendor(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err)
{
    if (!gpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_get_vendor");
        return -1;
    }
    char *s = strdup(gpu->vendor ? gpu->vendor : "Unknown");
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        "zenctl_gpu_get_vendor");
        return -1;
    }
    *out = s;
    return 0;
}

/* ── Temperature ─────────────────────────────────────────────────── */

int zenctl_gpu_get_temp(zenctl_gpu_t *gpu, int64_t *out_millideg, zenctl_err_t *err)
{
    if (!gpu || !out_millideg) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_get_temp");
        return -1;
    }
    const char *hw = gpu_hwmon(gpu, err);
    if (!hw) return -1;
    char path[8192];
    snprintf(path, sizeof(path), "%s/temp1_input", hw);
    return zenctl__read_file_i64(path, out_millideg, err);
}

/* ── Fan control ─────────────────────────────────────────────────── */

int zenctl_gpu_get_fan_pwm(zenctl_gpu_t *gpu, int *out, zenctl_err_t *err)
{
    if (!gpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_get_fan_pwm");
        return -1;
    }
    const char *hw = gpu_hwmon(gpu, err);
    if (!hw) return -1;
    char path[8192];
    snprintf(path, sizeof(path), "%s/pwm1", hw);
    int64_t v = 0;
    if (zenctl__read_file_i64(path, &v, err) != 0) return -1;
    *out = (int)v;
    return 0;
}

int zenctl_gpu_set_fan_pwm(zenctl_gpu_t *gpu, int pwm, zenctl_err_t *err)
{
    if (!gpu) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu",
                        "zenctl_gpu_set_fan_pwm");
        return -1;
    }
    if (pwm < 0 || pwm > 255) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "pwm out of range [0,255]", "zenctl_gpu_set_fan_pwm");
        return -1;
    }
    const char *hw = gpu_hwmon(gpu, err);
    if (!hw) return -1;
    char path[8192];
    snprintf(path, sizeof(path), "%s/pwm1", hw);
    return zenctl__write_file_i64(path, pwm, err);
}

int zenctl_gpu_get_fan_auto(zenctl_gpu_t *gpu, bool *out, zenctl_err_t *err)
{
    if (!gpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_get_fan_auto");
        return -1;
    }
    const char *hw = gpu_hwmon(gpu, err);
    if (!hw) return -1;
    char path[8192];
    snprintf(path, sizeof(path), "%s/pwm1_enable", hw);
    int64_t v = 0;
    if (zenctl__read_file_i64(path, &v, err) != 0) return -1;
    /* 2 (and above) = automatic; 1 = manual; 0 = full speed / no control. */
    *out = (v >= 2);
    return 0;
}

int zenctl_gpu_set_fan_auto(zenctl_gpu_t *gpu, bool auto_mode, zenctl_err_t *err)
{
    if (!gpu) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu",
                        "zenctl_gpu_set_fan_auto");
        return -1;
    }
    const char *hw = gpu_hwmon(gpu, err);
    if (!hw) return -1;
    char path[8192];
    snprintf(path, sizeof(path), "%s/pwm1_enable", hw);
    return zenctl__write_file_i64(path, auto_mode ? 2 : 1, err);
}

/* ── Power ───────────────────────────────────────────────────────── */

int zenctl_gpu_get_power(zenctl_gpu_t *gpu, int64_t *out_microwatts, zenctl_err_t *err)
{
    if (!gpu || !out_microwatts) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_get_power");
        return -1;
    }
    const char *hw = gpu_hwmon(gpu, err);
    if (!hw) return -1;
    /* amdgpu exports power1_average; some other drivers export power1_input. */
    char path[8192];
    snprintf(path, sizeof(path), "%s/power1_average", hw);
    if (zenctl__read_file_i64(path, out_microwatts, NULL) == 0)
        return 0;
    snprintf(path, sizeof(path), "%s/power1_input", hw);
    return zenctl__read_file_i64(path, out_microwatts, err);
}

/* ── Frequency ───────────────────────────────────────────────────── */

int zenctl_gpu_get_freq(zenctl_gpu_t *gpu, int64_t *out_mhz, zenctl_err_t *err)
{
    if (!gpu || !out_mhz) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_get_freq");
        return -1;
    }
    const char *hw = gpu_hwmon(gpu, err);
    if (!hw) return -1;
    char path[8192];
    snprintf(path, sizeof(path), "%s/freq1_input", hw);
    int64_t v = 0;
    if (zenctl__read_file_i64(path, &v, err) != 0) return -1;
    /* AMDGPU exports freq1_input in kHz per the kernel ABI; some drivers
     * report Hz. Heuristic: values > 100000 are kHz (since 100 GHz in kHz
     * is 1e8, unreachable). Divide to MHz. */
    if (v > 100000) v /= 1000;
    *out_mhz = v;
    return 0;
}

/* ── AMDGPU-specific ─────────────────────────────────────────────── */

int zenctl_gpu_amdgpu_get_power_profile(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err)
{
    if (!gpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_amdgpu_get_power_profile");
        return -1;
    }
    if (gpu_amdgpu_check(gpu, err) != 0) return -1;

    char path[8192];
    snprintf(path, sizeof(path), "%s/pp_power_profile_mode", gpu->device_path);
    FILE *f = fopen(path, "r");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), path);
        return -1;
    }
    char line[512];
    char *result = NULL;
    while (fgets(line, sizeof(line), f) != NULL) {
        /* Format: "  3  VIDEO  *\n". The line marked with trailing
         * '*' is the active profile. */
        char *star = strrchr(line, '*');
        if (!star) continue;
        while (star >= line && (isspace((unsigned char)*star) || *star == '*'))
            *star-- = '\0';
        /* Skip leading whitespace, then the index token, then more
         * whitespace; what's left is the profile name. */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        while (*p && !isspace((unsigned char)*p)) p++;  /* skip index */
        while (*p && isspace((unsigned char)*p)) p++;   /* skip space */
        if (*p) {
            result = strdup(p);
            break;
        }
    }
    fclose(f);
    if (!result) {
        zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                        "no active power profile found "
                        "(may require DPM level=manual)",
                        "zenctl_gpu_amdgpu_get_power_profile");
        return -1;
    }
    *out = result;
    return 0;
}

int zenctl_gpu_amdgpu_set_power_profile(zenctl_gpu_t *gpu, const char *profile, zenctl_err_t *err)
{
    if (!gpu || !profile) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or profile",
                        "zenctl_gpu_amdgpu_set_power_profile");
        return -1;
    }
    if (gpu_amdgpu_check(gpu, err) != 0) return -1;

    /* Lookup the index of the named profile in pp_power_profile_mode. */
    char path[8192];
    snprintf(path, sizeof(path), "%s/pp_power_profile_mode", gpu->device_path);
    FILE *f = fopen(path, "r");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), path);
        return -1;
    }
    char line[512];
    int index = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!isdigit((unsigned char)*p)) continue;
        long idx = strtol(p, &p, 10);
        while (*p && isspace((unsigned char)*p)) p++;
        char name[64];
        size_t i = 0;
        while (*p && !isspace((unsigned char)*p) && i < sizeof(name) - 1)
            name[i++] = *p++;
        name[i] = '\0';
        if (strcmp(name, profile) == 0) {
            index = (int)idx;
            break;
        }
    }
    fclose(f);
    if (index < 0) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "profile=%s", profile);
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "unknown power profile name", ctx);
        return -1;
    }

    /* Writing the index requires power_dpm_force_performance_level=manual. */
    char dpm_path[8192];
    snprintf(dpm_path, sizeof(dpm_path),
             "%s/power_dpm_force_performance_level", gpu->device_path);
    char *level = read_string_alloc(dpm_path, NULL);
    if (level && strcmp(level, "manual") != 0) {
        free(level);
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "power_dpm_force_performance_level must be 'manual' "
                        "to select a power profile",
                        "zenctl_gpu_amdgpu_set_power_profile");
        return -1;
    }
    free(level);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", index);
    return zenctl__write_file_string(path, buf, err);
}

int zenctl_gpu_amdgpu_get_dpm_state(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err)
{
    if (!gpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_amdgpu_get_dpm_state");
        return -1;
    }
    if (gpu_amdgpu_check(gpu, err) != 0) return -1;

    char path[8192];
    snprintf(path, sizeof(path),
             "%s/power_dpm_force_performance_level", gpu->device_path);
    char *s = read_string_alloc(path, err);
    if (!s) return -1;
    *out = s;
    return 0;
}

int zenctl_gpu_amdgpu_get_busy_percent(zenctl_gpu_t *gpu, int *out, zenctl_err_t *err)
{
    if (!gpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL gpu or out",
                        "zenctl_gpu_amdgpu_get_busy_percent");
        return -1;
    }
    if (gpu_amdgpu_check(gpu, err) != 0) return -1;

    char path[8192];
    snprintf(path, sizeof(path), "%s/gpu_busy_percent", gpu->device_path);
    int64_t v = 0;
    if (zenctl__read_file_i64(path, &v, err) != 0) return -1;
    if (v < 0 || v > 100) {
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "gpu_busy_percent out of range",
                        "zenctl_gpu_amdgpu_get_busy_percent");
        return -1;
    }
    *out = (int)v;
    return 0;
}
