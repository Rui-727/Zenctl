/* power.c - power domain implementation
 *
 * Backed by /sys/power/state, /sys/power/mem_sleep, per-device
 * /sys/devices/.../power/, and /sys/class/power_supply/. ACPI wake
 * sources come from /proc/acpi/wakeup (legacy but still works).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>

#include "zenctl/internal.h"
#include "zenctl/power.h"


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

/* ── Sleep states ────────────────────────────────────────────────── */

static const char *sleep_state_name(zenctl_sleep_state_t state)
{
    switch (state) {
    case ZENCTL_SLEEP_FREEZE:  return "freeze";
    case ZENCTL_SLEEP_MEM:     return "mem";
    case ZENCTL_SLEEP_DISK:    return "disk";
    case ZENCTL_SLEEP_STANDBY: return "standby";
    default:                   return NULL;
    }
}

int zenctl_power_get_supported_states(char **out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out",
                        "zenctl_power_get_supported_states");
        return -1;
    }
    char *s = read_string_alloc("/sys/power/state", err);
    if (!s) return -1;
    *out = s;
    return 0;
}

int zenctl_power_suspend(zenctl_sleep_state_t state, zenctl_err_t *err)
{
    const char *name = sleep_state_name(state);
    if (!name) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "invalid sleep state",
                        "zenctl_power_suspend");
        return -1;
    }
    /* Verify the state is supported before attempting. */
    char *avail = read_string_alloc("/sys/power/state", err);
    if (!avail) return -1;
    bool supported = false;
    char *save = NULL;
    for (char *tok = strtok_r(avail, " \t\n", &save);
         tok; tok = strtok_r(NULL, " \t\n", &save)) {
        if (strcmp(tok, name) == 0) { supported = true; break; }
    }
    free(avail);
    if (!supported) {
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "state=%s", name);
        zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                        "sleep state not supported", ctx);
        return -1;
    }

    sync();  /* best-effort sync before suspend */

    return zenctl__write_file_string("/sys/power/state", name, err);
}

/* ── mem_sleep mode ──────────────────────────────────────────────── */

int zenctl_power_get_mem_sleep_mode(char **out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out",
                        "zenctl_power_get_mem_sleep_mode");
        return -1;
    }
    char *raw = read_string_alloc("/sys/power/mem_sleep", err);
    if (!raw) return -1;
    /* The active mode is wrapped in [brackets], e.g. "s2idle [deep]". */
    char *start = strchr(raw, '[');
    char *end = strchr(raw, ']');
    char *result = NULL;
    if (start && end && end > start) {
        size_t len = (size_t)(end - start - 1);
        result = malloc(len + 1);
        if (result) {
            memcpy(result, start + 1, len);
            result[len] = '\0';
        }
    } else {
        /* No brackets: return the first token. */
        char *save = NULL;
        char *tok = strtok_r(raw, " \t\n", &save);
        result = strdup(tok ? tok : "");
    }
    free(raw);
    if (!result) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "malloc/strdup failed",
                        "zenctl_power_get_mem_sleep_mode");
        return -1;
    }
    *out = result;
    return 0;
}

int zenctl_power_set_mem_sleep_mode(const char *mode, zenctl_err_t *err)
{
    if (!mode) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL mode",
                        "zenctl_power_set_mem_sleep_mode");
        return -1;
    }
    return zenctl__write_file_string("/sys/power/mem_sleep", mode, err);
}

/* ── Runtime PM ──────────────────────────────────────────────────── */

int zenctl_power_get_runtime_pm(const char *dev_path, char **out, zenctl_err_t *err)
{
    if (!dev_path || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL dev_path or out",
                        "zenctl_power_get_runtime_pm");
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/power/control", dev_path);
    char *s = read_string_alloc(path, err);
    if (!s) return -1;
    *out = s;
    return 0;
}

int zenctl_power_set_runtime_pm(const char *dev_path, const char *mode, zenctl_err_t *err)
{
    if (!dev_path || !mode) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL dev_path or mode",
                        "zenctl_power_set_runtime_pm");
        return -1;
    }
    if (strcmp(mode, "auto") != 0 && strcmp(mode, "on") != 0) {
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "mode=%s (expected auto|on)", mode);
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid runtime PM mode", ctx);
        return -1;
    }
    char path[8192];
    snprintf(path, sizeof(path), "%s/power/control", dev_path);
    return zenctl__write_file_string(path, mode, err);
}

/* ── Battery enumeration ─────────────────────────────────────────── */

/* Scan /sys/class/power_supply/ and collect the names of supplies
 * whose `type` file equals `want_type`. Returns a malloced array of
 * malloced strings, NULL-terminated, with the count written to
 * *count. Returns NULL on error. */
static char **list_supplies_by_type(const char *want_type, int *count,
                                    zenctl_err_t *err)
{
    *count = 0;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), "/sys/class/power_supply");
        return NULL;
    }
    char **list = NULL;
    int cap = 0, n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char type_path[8192];
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/power_supply/%s/type", de->d_name);
        char *t = read_string_alloc(type_path, NULL);
        if (!t) continue;
        bool match = (strcmp(t, want_type) == 0);
        free(t);
        if (!match) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            char **nl = realloc(list, sizeof(char *) * (cap + 1));
            if (!nl) {
                for (int i = 0; i < n; i++) free(list[i]);
                free(list);
                closedir(d);
                zenctl__set_err(err, ZENCTL_ERR_NOMEM, "realloc failed",
                                "list_supplies_by_type");
                return NULL;
            }
            list = nl;
        }
        list[n] = strdup(de->d_name);
        if (!list[n]) {
            for (int i = 0; i < n; i++) free(list[i]);
            free(list);
            closedir(d);
            zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                            "list_supplies_by_type");
            return NULL;
        }
        n++;
    }
    closedir(d);
    if (list) list[n] = NULL;
    *count = n;
    return list;
}

/* Return the name (malloced) of the Nth battery or NULL. */
static char *nth_battery_name(int index, zenctl_err_t *err)
{
    int n = 0;
    char **list = list_supplies_by_type("Battery", &n, err);
    if (!list) return NULL;
    if (index < 0 || index >= n) {
        char ctx[64];
        snprintf(ctx, sizeof(ctx),
                 "battery index %d out of range [0,%d)", index, n);
        zenctl__set_err(err, ZENCTL_ERR_ENOENT, "battery not found", ctx);
        for (int i = 0; i < n; i++) free(list[i]);
        free(list);
        return NULL;
    }
    /* Stable order: sort the list so index lookups are deterministic. */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(list[i], list[j]) > 0) {
                char *tmp = list[i]; list[i] = list[j]; list[j] = tmp;
            }
        }
    }
    char *out = strdup(list[index]);
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    if (!out)
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        "nth_battery_name");
    return out;
}

/* Read an int64 from /sys/class/power_supply/<name>/<key> into *out.
 * On ENOENT, *out is left unchanged. */
static void read_supply_int64(const char *name, const char *key, int64_t *out)
{
    char path[8192];
    snprintf(path, sizeof(path),
             "/sys/class/power_supply/%s/%s", name, key);
    int64_t v = 0;
    if (zenctl__read_file_i64(path, &v, NULL) == 0) *out = v;
}

static void read_supply_string(const char *name, const char *key,
                               char *out, size_t outsz)
{
    char path[8192];
    snprintf(path, sizeof(path),
             "/sys/class/power_supply/%s/%s", name, key);
    char *s = read_string_alloc(path, NULL);
    if (s) {
        snprintf(out, outsz, "%s", s);
        free(s);
    } else {
        out[0] = '\0';
    }
}

int zenctl_power_battery_count(int *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out",
                        "zenctl_power_battery_count");
        return -1;
    }
    int n = 0;
    char **list = list_supplies_by_type("Battery", &n, err);
    if (!list) return -1;
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    *out = n;
    return 0;
}

int zenctl_power_battery_get(int index, zenctl_battery_t *out, zenctl_err_t *err)
{
    if (!out || index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out or negative index",
                        "zenctl_power_battery_get");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->capacity = -1;
    out->cycle_count = -1;
    out->charge_full = -1;
    out->charge_full_design = -1;
    out->charge_now = -1;
    out->current_now = 0;
    out->voltage_now = 0;

    char *name = nth_battery_name(index, err);
    if (!name) return -1;
    snprintf(out->name, sizeof(out->name), "%s", name);

    read_supply_string(name, "status", out->status, sizeof(out->status));
    read_supply_string(name, "technology", out->technology,
                       sizeof(out->technology));

    int64_t v = 0;
    read_supply_int64(name, "capacity", &v);
    if (v >= 0) out->capacity = (int)v;
    v = 0;
    read_supply_int64(name, "cycle_count", &v);
    if (v >= 0) out->cycle_count = (int)v;
    read_supply_int64(name, "charge_full", &out->charge_full);
    read_supply_int64(name, "charge_full_design", &out->charge_full_design);
    read_supply_int64(name, "charge_now", &out->charge_now);
    read_supply_int64(name, "current_now", &out->current_now);
    read_supply_int64(name, "voltage_now", &out->voltage_now);

    free(name);
    return 0;
}

/* ── Charge thresholds ───────────────────────────────────────────── */

int zenctl_power_battery_get_charge_start(int index, int *out_percent, zenctl_err_t *err)
{
    if (!out_percent || index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out or negative index",
                        "zenctl_power_battery_get_charge_start");
        return -1;
    }
    char *name = nth_battery_name(index, err);
    if (!name) return -1;
    char path[8192];
    snprintf(path, sizeof(path),
             "/sys/class/power_supply/%s/charge_control_start_threshold", name);
    free(name);
    int64_t v = 0;
    if (zenctl__read_file_i64(path, &v, err) != 0) {
        if (err && err->code == ZENCTL_OK)
            zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                            "charge start threshold not supported", path);
        return -1;
    }
    if (v < 0 || v > 100) {
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "threshold=%lld", (long long)v);
        zenctl__set_err(err, ZENCTL_ERR_EIO, "threshold out of range", ctx);
        return -1;
    }
    *out_percent = (int)v;
    return 0;
}

int zenctl_power_battery_set_charge_start(int index, int percent, zenctl_err_t *err)
{
    if (index < 0 || percent < 0 || percent > 100) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid argument (percent must be 0..100)",
                        "zenctl_power_battery_set_charge_start");
        return -1;
    }
    char *name = nth_battery_name(index, err);
    if (!name) return -1;
    char path[8192];
    snprintf(path, sizeof(path),
             "/sys/class/power_supply/%s/charge_control_start_threshold", name);
    free(name);
    return zenctl__write_file_i64(path, percent, err);
}

int zenctl_power_battery_get_charge_end(int index, int *out_percent, zenctl_err_t *err)
{
    if (!out_percent || index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out or negative index",
                        "zenctl_power_battery_get_charge_end");
        return -1;
    }
    char *name = nth_battery_name(index, err);
    if (!name) return -1;
    char path[8192];
    snprintf(path, sizeof(path),
             "/sys/class/power_supply/%s/charge_control_end_threshold", name);
    free(name);
    int64_t v = 0;
    if (zenctl__read_file_i64(path, &v, err) != 0) {
        if (err && err->code == ZENCTL_OK)
            zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                            "charge end threshold not supported", path);
        return -1;
    }
    if (v < 0 || v > 100) {
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "threshold=%lld", (long long)v);
        zenctl__set_err(err, ZENCTL_ERR_EIO, "threshold out of range", ctx);
        return -1;
    }
    *out_percent = (int)v;
    return 0;
}

int zenctl_power_battery_set_charge_end(int index, int percent, zenctl_err_t *err)
{
    if (index < 0 || percent < 0 || percent > 100) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid argument (percent must be 0..100)",
                        "zenctl_power_battery_set_charge_end");
        return -1;
    }
    char *name = nth_battery_name(index, err);
    if (!name) return -1;
    char path[8192];
    snprintf(path, sizeof(path),
             "/sys/class/power_supply/%s/charge_control_end_threshold", name);
    free(name);
    return zenctl__write_file_i64(path, percent, err);
}

/* ── AC adapter ──────────────────────────────────────────────────── */

int zenctl_power_ac_online(bool *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out",
                        "zenctl_power_ac_online");
        return -1;
    }
    int n = 0;
    char **list = list_supplies_by_type("Mains", &n, err);
    if (!list) return -1;
    bool online = false;
    for (int i = 0; i < n; i++) {
        char path[8192];
        snprintf(path, sizeof(path),
                 "/sys/class/power_supply/%s/online", list[i]);
        int64_t v = 0;
        if (zenctl__read_file_i64(path, &v, NULL) == 0 && v == 1) {
            online = true;
            break;
        }
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    *out = online;
    return 0;
}

/* ── Wake sources ────────────────────────────────────────────────── */

int zenctl_power_get_wakeup_devices(char ***out_list, int *out_count, zenctl_err_t *err)
{
    if (!out_list || !out_count) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out_list or out_count",
                        "zenctl_power_get_wakeup_devices");
        return -1;
    }
    *out_list = NULL;
    *out_count = 0;
    FILE *f = fopen("/proc/acpi/wakeup", "r");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno),
                        "/proc/acpi/wakeup (ACPI-only / may be absent)");
        return -1;
    }
    char line[512];
    char **list = NULL;
    int cap = 0, n = 0;
    bool first = true;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (first) { first = false; continue; }
        /* Each row: "Device  S-state  Status  Sysfs node". */
        char *save = NULL;
        char *tok = strtok_r(line, " \t\n", &save);
        if (!tok) continue;
        /* Skip separator lines like "---" (some kernels print those). */
        if (tok[0] == '-' || !isalpha((unsigned char)tok[0])) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **nl = realloc(list, sizeof(char *) * (cap + 1));
            if (!nl) {
                for (int i = 0; i < n; i++) free(list[i]);
                free(list);
                fclose(f);
                zenctl__set_err(err, ZENCTL_ERR_NOMEM, "realloc failed",
                                "zenctl_power_get_wakeup_devices");
                return -1;
            }
            list = nl;
        }
        list[n] = strdup(tok);
        if (!list[n]) {
            for (int i = 0; i < n; i++) free(list[i]);
            free(list);
            fclose(f);
            zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                            "zenctl_power_get_wakeup_devices");
            return -1;
        }
        n++;
    }
    fclose(f);
    if (list) list[n] = NULL;
    *out_list = list;
    *out_count = n;
    return 0;
}

/* ── Wake source toggle ──────────────────────────────────────────── */

/* /proc/acpi/wakeup rows look like:
 *   Device  S-state   Status   Sysfs node
 *   LID       S4    *enabled   platform:PNP0C0D:00
 *   PEG0      S4    *disabled  pci:0000:00:01.0
 *
 * Writing the device name (e.g. "PEG0") to /proc/acpi/wakeup toggles
 * its enabled state. To set an absolute state we parse the current
 * state and only write if it differs. */
int zenctl_power_set_wakeup(const char *device, bool enabled, zenctl_err_t *err)
{
    if (!device || !*device) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL or empty device",
                        "zenctl_power_set_wakeup");
        return -1;
    }
    /* Reject anything that looks like path traversal or whitespace.
     * ACPI device names are short alphanumeric tokens (LID, PEG0,
     * PNP0C0D, ...). */
    for (const char *c = device; *c; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch == '/' || ch == '\n' || ch == '\r' || ch == '\t' ||
            ch == ' ') {
            zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                            "invalid device name", device);
            return -1;
        }
    }

    /* Read the current wakeup table. /proc/acpi/wakeup is typically
     * a few KB at most (one row per wake source). */
    char wbuf[16384];
    if (zenctl__read_file_string("/proc/acpi/wakeup", wbuf, sizeof(wbuf),
                                 err) != 0)
        return -1;

    /* Parse row by row. The first line is a header; some kernels
     * also print a "---" separator. Each remaining row's first
     * whitespace-delimited token is the device name, third token is
     * the status ("*enabled" or "*disabled"). */
    bool found = false;
    bool currently_enabled = false;
    char *save_line = NULL;
    for (char *line = strtok_r(wbuf, "\n", &save_line);
         line; line = strtok_r(NULL, "\n", &save_line)) {
        char *save_tok = NULL;
        char *dev = strtok_r(line, " \t", &save_tok);
        if (!dev) continue;
        if (dev[0] == '-' || !isalpha((unsigned char)dev[0])) continue;
        if (strcmp(dev, device) != 0) continue;
        /* S-state (skip) + status */
        (void)strtok_r(NULL, " \t", &save_tok);
        char *status = strtok_r(NULL, " \t", &save_tok);
        if (!status) continue;
        found = true;
        currently_enabled = (strcmp(status, "*enabled") == 0);
        break;
    }

    if (!found) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "device=%s", device);
        zenctl__set_err(err, ZENCTL_ERR_ENOENT,
                        "device not in /proc/acpi/wakeup", ctx);
        return -1;
    }

    /* Already in the desired state: nothing to do. */
    if (currently_enabled == enabled) return 0;

    /* Toggle by writing the device name. */
    return zenctl__write_file_string("/proc/acpi/wakeup", device, err);
}
