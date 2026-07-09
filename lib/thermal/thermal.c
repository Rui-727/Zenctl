/* thermal.c - thermal domain implementation
 *
 * Backed by /sys/class/thermal and /sys/class/hwmon. Covers thermal
 * zones, trip points, cooling devices, and standalone hwmon sensors
 * and fan controls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>

#include "zenctl/internal.h"
#include "zenctl/thermal.h"


/* Path construction uses snprintf with PATH_MAX-sized buffers.
 * Real paths are <100 chars; theoretical truncation warnings
 * from -Wformat-truncation are noise here. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* ── local helpers ───────────────────────────────────────────────── */

static char *read_string_alloc(const char *path, zenctl_err_t *err)
{
    char buf[4096];
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return NULL;
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

/* Count entries in a directory whose name matches <prefix><digits>. */
static int count_dir_entries(const char *dir, const char *prefix,
                             zenctl_err_t *err)
{
    DIR *d = opendir(dir);
    if (!d) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), dir);
        return -1;
    }
    int count = 0;
    size_t plen = strlen(prefix);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, prefix, plen) != 0) continue;
        const char *p = de->d_name + plen;
        if (!isdigit((unsigned char)*p)) continue;
        bool all_digits = true;
        for (; *p; p++)
            if (!isdigit((unsigned char)*p)) { all_digits = false; break; }
        if (all_digits) count++;
    }
    closedir(d);
    return count;
}

/* ── Thermal zone object ─────────────────────────────────────────── */

struct zenctl_thermal {
    char *zone_name;   /* e.g. "thermal_zone0" */
    char *zone_path;   /* /sys/class/thermal/thermal_zone0 */
};

static bool valid_zone_name(const char *name)
{
    if (!name) return false;
    if (strncmp(name, "thermal_zone", 12) != 0) return false;
    if (name[12] == '\0') return false;
    for (const char *p = name + 12; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
    return true;
}

zenctl_thermal_t *zenctl_thermal_open(const char *zone_name, zenctl_err_t *err)
{
    if (!valid_zone_name(zone_name)) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid zone name (expected thermal_zoneN)",
                        "zenctl_thermal_open");
        return NULL;
    }
    char path[8192];
    snprintf(path, sizeof(path), "/sys/class/thermal/%s", zone_name);
    /* Verify the zone exists by probing temp. */
    char temp_path[8192];
    snprintf(temp_path, sizeof(temp_path), "%s/temp", path);
    char buf[64];
    if (zenctl__read_file_string(temp_path, buf, sizeof(buf), err) != 0) {
        if (err && err->code == ZENCTL_OK)
            zenctl__set_err(err, ZENCTL_ERR_ENOENT,
                            "thermal zone not found", temp_path);
        return NULL;
    }
    zenctl_thermal_t *tz = calloc(1, sizeof(*tz));
    if (!tz) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed",
                        "zenctl_thermal_open");
        return NULL;
    }
    tz->zone_name = strdup(zone_name);
    tz->zone_path = strdup(path);
    if (!tz->zone_name || !tz->zone_path) {
        free(tz->zone_name); free(tz->zone_path); free(tz);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        "zenctl_thermal_open");
        return NULL;
    }
    return tz;
}

void zenctl_thermal_close(zenctl_thermal_t *tz)
{
    if (!tz) return;
    free(tz->zone_name);
    free(tz->zone_path);
    free(tz);
}

/* ── Temperature ─────────────────────────────────────────────────── */

int zenctl_thermal_get_temp(zenctl_thermal_t *tz, int64_t *out, zenctl_err_t *err)
{
    if (!tz || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or out",
                        "zenctl_thermal_get_temp");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/temp", tz->zone_path);
    return zenctl__read_file_i64(path, out, err);
}

/* ── Zone info ───────────────────────────────────────────────────── */

int zenctl_thermal_get_type(zenctl_thermal_t *tz, char **out, zenctl_err_t *err)
{
    if (!tz || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or out",
                        "zenctl_thermal_get_type");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/type", tz->zone_path);
    char *s = read_string_alloc(path, err);
    if (!s) return -1;
    *out = s;
    return 0;
}

int zenctl_thermal_get_policy(zenctl_thermal_t *tz, char **out, zenctl_err_t *err)
{
    if (!tz || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or out",
                        "zenctl_thermal_get_policy");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/policy", tz->zone_path);
    char *s = read_string_alloc(path, err);
    if (!s) return -1;
    *out = s;
    return 0;
}

int zenctl_thermal_set_policy(zenctl_thermal_t *tz, const char *policy, zenctl_err_t *err)
{
    if (!tz || !policy) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or policy",
                        "zenctl_thermal_set_policy");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/policy", tz->zone_path);
    return zenctl__write_file_string(path, policy, err);
}

int zenctl_thermal_list_policies(zenctl_thermal_t *tz, char **out, zenctl_err_t *err)
{
    if (!tz || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or out",
                        "zenctl_thermal_list_policies");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/available_policies", tz->zone_path);
    char *s = read_string_alloc(path, err);
    if (!s) return -1;
    *out = s;
    return 0;
}

/* ── Zone mode ───────────────────────────────────────────────────── */

int zenctl_thermal_get_mode(zenctl_thermal_t *tz, bool *enabled, zenctl_err_t *err)
{
    if (!tz || !enabled) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or enabled",
                        "zenctl_thermal_get_mode");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/mode", tz->zone_path);
    char *s = read_string_alloc(path, err);
    if (!s) return -1;
    *enabled = (strcmp(s, "enabled") == 0);
    free(s);
    return 0;
}

int zenctl_thermal_set_mode(zenctl_thermal_t *tz, bool enabled, zenctl_err_t *err)
{
    if (!tz) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz",
                        "zenctl_thermal_set_mode");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/mode", tz->zone_path);
    return zenctl__write_file_string(path, enabled ? "enabled" : "disabled", err);
}

/* ── Trip points ─────────────────────────────────────────────────── */

int zenctl_thermal_get_trip_count(zenctl_thermal_t *tz, int *out, zenctl_err_t *err)
{
    if (!tz || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or out",
                        "zenctl_thermal_get_trip_count");
        return -1;
    }
    /* Trip points are numbered densely from 0; stop at the first gap. */
    int count = 0;
    for (;;) {
        char path[8192];
        snprintf(path, sizeof(path), "%s/trip_point_%d_temp",
                 tz->zone_path, count);
        if (access(path, F_OK) != 0) break;
        count++;
    }
    *out = count;
    return 0;
}

int zenctl_thermal_get_trip(zenctl_thermal_t *tz, int index, zenctl_trip_t *out, zenctl_err_t *err)
{
    if (!tz || !out || index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz/out or negative index",
                        "zenctl_thermal_get_trip");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->index = index;
    out->hysteresis = -1;  /* sentinel: not set */

    char path[8192];
    snprintf(path, sizeof(path), "%s/trip_point_%d_temp", tz->zone_path, index);
    if (zenctl__read_file_i64(path, &out->temp, err) != 0) return -1;

    snprintf(path, sizeof(path), "%s/trip_point_%d_type", tz->zone_path, index);
    char *t = read_string_alloc(path, NULL);
    if (t) {
        snprintf(out->type, sizeof(out->type), "%s", t);
        free(t);
    } else {
        snprintf(out->type, sizeof(out->type), "unknown");
    }

    /* Hysteresis is reported in whole °C, not m°C. Convert to m°C. */
    snprintf(path, sizeof(path), "%s/trip_point_%d_hyst", tz->zone_path, index);
    int64_t h = 0;
    zenctl_err_t local_err;
    memset(&local_err, 0, sizeof(local_err));
    if (zenctl__read_file_i64(path, &h, &local_err) == 0) {
        out->hysteresis = h * 1000;
    } else if (local_err.code != ZENCTL_ERR_ENOENT) {
        /* Present but unreadable: surface the error. */
        if (err) *err = local_err;
        return -1;
    }
    return 0;
}

/* ── Cooling devices ─────────────────────────────────────────────── */

/* Parse the cooling device index from a zone's cdevN symlink.
 * Returns 0 on success and writes the cdev index (e.g. 5 for
 * cooling_device5) into *out_index. */
static int zone_cdev_index(zenctl_thermal_t *tz, int zone_cdev_index,
                           int *out_index, zenctl_err_t *err)
{
    char link[8192];
    snprintf(link, sizeof(link), "%s/cdev%d", tz->zone_path, zone_cdev_index);
    char *target = readlink_basename(link, err);
    if (!target) return -1;
    int idx = -1;
    if (strncmp(target, "cooling_device", 14) == 0)
        idx = atoi(target + 14);
    free(target);
    if (idx < 0) {
        zenctl__set_err(err, ZENCTL_ERR_INTERNAL,
                        "unrecognised cooling device symlink target",
                        "zone_cdev_index");
        return -1;
    }
    *out_index = idx;
    return 0;
}

int zenctl_thermal_get_cooling_count(zenctl_thermal_t *tz, int *out, zenctl_err_t *err)
{
    if (!tz || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or out",
                        "zenctl_thermal_get_cooling_count");
        return -1;
    }
    int n = count_dir_entries(tz->zone_path, "cdev", err);
    if (n < 0) return -1;
    *out = n;
    return 0;
}

int zenctl_thermal_get_cooling(zenctl_thermal_t *tz, int index, zenctl_cooling_t *out, zenctl_err_t *err)
{
    if (!tz || !out || index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL tz/out or negative index",
                        "zenctl_thermal_get_cooling");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->index = index;
    out->cur_state = -1;
    out->max_state = -1;

    int cdev_idx = 0;
    if (zone_cdev_index(tz, index, &cdev_idx, err) != 0) return -1;

    char cdev_path[8192];
    snprintf(cdev_path, sizeof(cdev_path),
             "/sys/class/thermal/cooling_device%d", cdev_idx);

    char path[8192];
    snprintf(path, sizeof(path), "%s/type", cdev_path);
    char *t = read_string_alloc(path, NULL);
    if (t) {
        snprintf(out->type, sizeof(out->type), "%s", t);
        free(t);
    }

    int64_t v = 0;
    snprintf(path, sizeof(path), "%s/cur_state", cdev_path);
    if (zenctl__read_file_i64(path, &v, NULL) == 0) out->cur_state = (int)v;

    snprintf(path, sizeof(path), "%s/max_state", cdev_path);
    if (zenctl__read_file_i64(path, &v, NULL) == 0) out->max_state = (int)v;

    if (out->max_state < 0) {
        char ctx[64];
        snprintf(ctx, sizeof(ctx),
                 "cooling_device%d missing max_state", cdev_idx);
        zenctl__set_err(err, ZENCTL_ERR_ENOENT,
                        "cooling device not found", ctx);
        return -1;
    }
    return 0;
}

int zenctl_thermal_set_cooling_state(zenctl_thermal_t *tz, int index, int state, zenctl_err_t *err)
{
    if (!tz || index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL tz or negative index",
                        "zenctl_thermal_set_cooling_state");
        return -1;
    }
    int cdev_idx = 0;
    if (zone_cdev_index(tz, index, &cdev_idx, err) != 0) return -1;

    /* Range-check against max_state when available. */
    char max_path[8192];
    snprintf(max_path, sizeof(max_path),
             "/sys/class/thermal/cooling_device%d/max_state", cdev_idx);
    int64_t maxv = 0;
    if (zenctl__read_file_i64(max_path, &maxv, NULL) == 0) {
        if (state < 0 || state > maxv) {
            char ctx[128];
            snprintf(ctx, sizeof(ctx),
                     "state %d out of range [0, %lld]", state, (long long)maxv);
            zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                            "cooling state out of range", ctx);
            return -1;
        }
    }

    char path[8192];
    snprintf(path, sizeof(path),
             "/sys/class/thermal/cooling_device%d/cur_state", cdev_idx);
    return zenctl__write_file_i64(path, state, err);
}

/* ── hwmon (standalone) ──────────────────────────────────────────── */

static int hwmon_path(int hwmon_index, char *out, size_t outsz)
{
    int n = snprintf(out, outsz, "/sys/class/hwmon/hwmon%d", hwmon_index);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

int zenctl_thermal_hwmon_count(int *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out",
                        "zenctl_thermal_hwmon_count");
        return -1;
    }
    int n = count_dir_entries("/sys/class/hwmon", "hwmon", err);
    if (n < 0) return -1;
    *out = n;
    return 0;
}

/* Read temp_max / temp_crit into out->temp_max / temp_crit. -1 if absent. */
static void load_temp_limits(const char *hwmon, int sensor, zenctl_hwmon_temp_t *out)
{
    out->temp_max = -1;
    out->temp_crit = -1;
    char path[8192];
    int64_t v = 0;
    snprintf(path, sizeof(path), "%s/temp%d_max", hwmon, sensor);
    if (zenctl__read_file_i64(path, &v, NULL) == 0) out->temp_max = v;
    snprintf(path, sizeof(path), "%s/temp%d_crit", hwmon, sensor);
    if (zenctl__read_file_i64(path, &v, NULL) == 0) out->temp_crit = v;
}

int zenctl_thermal_hwmon_get_temp(int hwmon_index, int sensor_index,
                                  zenctl_hwmon_temp_t *out, zenctl_err_t *err)
{
    if (!out || hwmon_index < 0 || sensor_index < 1) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out or negative index",
                        "zenctl_thermal_hwmon_get_temp");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->temp_max = -1;
    out->temp_crit = -1;

    char hwmon[8192];
    if (hwmon_path(hwmon_index, hwmon, sizeof(hwmon)) != 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "hwmon index too large",
                        "zenctl_thermal_hwmon_get_temp");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/temp%d_input", hwmon, sensor_index);
    if (zenctl__read_file_i64(path, &out->temp, err) != 0) return -1;

    snprintf(path, sizeof(path), "%s/name", hwmon);
    char *s = read_string_alloc(path, NULL);
    if (s) {
        snprintf(out->name, sizeof(out->name), "%s", s);
        free(s);
    }
    snprintf(path, sizeof(path), "%s/temp%d_label", hwmon, sensor_index);
    s = read_string_alloc(path, NULL);
    if (s) {
        snprintf(out->label, sizeof(out->label), "%s", s);
        free(s);
    }
    load_temp_limits(hwmon, sensor_index, out);
    return 0;
}

/* ── hwmon fan control ───────────────────────────────────────────── */

int zenctl_thermal_hwmon_get_fan_pwm(int hwmon_index, int fan_index, int *out, zenctl_err_t *err)
{
    if (!out || hwmon_index < 0 || fan_index < 1) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out or negative index",
                        "zenctl_thermal_hwmon_get_fan_pwm");
        return -1;
    }
    char hwmon[8192];
    if (hwmon_path(hwmon_index, hwmon, sizeof(hwmon)) != 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "hwmon index too large",
                        "zenctl_thermal_hwmon_get_fan_pwm");
        return -1;
    }
    char path[8192];
    int64_t v = 0;
    snprintf(path, sizeof(path), "%s/pwm%d", hwmon, fan_index);
    if (zenctl__read_file_i64(path, &v, err) != 0) return -1;
    *out = (int)v;
    return 0;
}

int zenctl_thermal_hwmon_set_fan_pwm(int hwmon_index, int fan_index, int pwm, zenctl_err_t *err)
{
    if (hwmon_index < 0 || fan_index < 1) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "negative index",
                        "zenctl_thermal_hwmon_set_fan_pwm");
        return -1;
    }
    if (pwm < 0 || pwm > 255) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE, "pwm out of range [0,255]",
                        "zenctl_thermal_hwmon_set_fan_pwm");
        return -1;
    }
    char hwmon[8192];
    if (hwmon_path(hwmon_index, hwmon, sizeof(hwmon)) != 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "hwmon index too large",
                        "zenctl_thermal_hwmon_set_fan_pwm");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/pwm%d", hwmon, fan_index);
    return zenctl__write_file_i64(path, pwm, err);
}

int zenctl_thermal_hwmon_get_fan_auto(int hwmon_index, int fan_index, bool *out, zenctl_err_t *err)
{
    if (!out || hwmon_index < 0 || fan_index < 1) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out or negative index",
                        "zenctl_thermal_hwmon_get_fan_auto");
        return -1;
    }
    char hwmon[8192];
    if (hwmon_path(hwmon_index, hwmon, sizeof(hwmon)) != 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "hwmon index too large",
                        "zenctl_thermal_hwmon_get_fan_auto");
        return -1;
    }
    char path[8192];
    int64_t v = 0;
    snprintf(path, sizeof(path), "%s/pwm%d_enable", hwmon, fan_index);
    if (zenctl__read_file_i64(path, &v, err) != 0) return -1;
    *out = (v >= 2);
    return 0;
}

int zenctl_thermal_hwmon_set_fan_auto(int hwmon_index, int fan_index, bool auto_mode, zenctl_err_t *err)
{
    if (hwmon_index < 0 || fan_index < 1) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "negative index",
                        "zenctl_thermal_hwmon_set_fan_auto");
        return -1;
    }
    char hwmon[8192];
    if (hwmon_path(hwmon_index, hwmon, sizeof(hwmon)) != 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "hwmon index too large",
                        "zenctl_thermal_hwmon_set_fan_auto");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/pwm%d_enable", hwmon, fan_index);
    return zenctl__write_file_i64(path, auto_mode ? 2 : 1, err);
}
