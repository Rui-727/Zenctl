/* thermal.c - zenctl thermal subcommand.
 *
 * Implements:
 *   zenctl thermal list
 *   zenctl thermal hwmon [fan <hwmon> <fan> [<0-255>]]
 *   zenctl thermal get {temp|type|policy|mode|trips|cooling} --zone Z
 *   zenctl thermal set {policy|mode|cooling} ...           --zone Z
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/thermal.h"

#include "../output.h"
#include "common.h"

/* ── Enumeration ────────────────────────────────────────────────── */

static int enum_indexed(const char *dir, const char *prefix,
                        int *out, int cap, int *count)
{
    *count = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    size_t plen = strlen(prefix);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, prefix, plen) != 0) continue;
        const char *p = de->d_name + plen;
        if (!isdigit((unsigned char)*p)) continue;
        bool pure = true;
        for (; *p; p++) if (!isdigit((unsigned char)*p)) { pure = false; break; }
        if (!pure) continue;
        if (*count >= cap) break;
        out[*count] = atoi(de->d_name + plen);
        (*count)++;
    }
    closedir(d);
    for (int i = 1; i < *count; i++) {
        int v = out[i], j = i;
        while (j > 0 && out[j - 1] > v) { out[j] = out[j - 1]; j--; }
        out[j] = v;
    }
    return 0;
}

static void zone_name(int idx, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "thermal_zone%d", idx);
}

/* ── thermal list ───────────────────────────────────────────────── */

static int thermal_list(bool json)
{
    int zones[64], n = 0;
    if (enum_indexed("/sys/class/thermal", "thermal_zone",
                     zones, 64, &n) != 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EIO,
                     "cannot read /sys/class/thermal", "thermal_list");
        return cli_err(json, &err);
    }
    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            out_json_field_int(&first, "index", zones[i]);
            char zn[32]; zone_name(zones[i], zn, sizeof(zn));
            out_json_field_string(&first, "zone", zn);
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            zenctl_thermal_t *tz = zenctl_thermal_open(zn, &err);
            if (tz) {
                char *s = NULL;
                if (zenctl_thermal_get_type(tz, &s, NULL) == 0) {
                    out_json_field_string(&first, "type", s);
                    free(s);
                }
                int64_t t = 0;
                if (zenctl_thermal_get_temp(tz, &t, NULL) == 0)
                    out_json_field_int(&first, "temp_mc", t);
                zenctl_thermal_close(tz);
            }
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"ZONE", "TYPE", "TEMP"};
        out_table_header(h, 3);
        for (int i = 0; i < n; i++) {
            char zn[32]; zone_name(zones[i], zn, sizeof(zn));
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            zenctl_thermal_t *tz = zenctl_thermal_open(zn, &err);
            char *type = NULL; int64_t t = -1;
            if (tz) {
                zenctl_thermal_get_type(tz, &type, NULL);
                zenctl_thermal_get_temp(tz, &t, NULL);
                zenctl_thermal_close(tz);
            }
            char tb[32];
            if (t >= 0) snprintf(tb, sizeof(tb), "%lld mC", (long long)t);
            else snprintf(tb, sizeof(tb), "-");
            const char *r[] = {zn, type ? type : "-", tb};
            out_table_row(r, 3);
            free(type);
        }
    }
    return 0;
}

/* ── thermal hwmon ──────────────────────────────────────────────── */

static int thermal_hwmon_list(bool json)
{
    int hs[64], n = 0;
    if (enum_indexed("/sys/class/hwmon", "hwmon", hs, 64, &n) != 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EIO,
                     "cannot read /sys/class/hwmon", "thermal_hwmon_list");
        return cli_err(json, &err);
    }
    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            out_json_field_int(&first, "hwmon", hs[i]);
            for (int s = 1; s < 16; s++) {
                zenctl_hwmon_temp_t t;
                memset(&t, 0, sizeof(t));
                if (zenctl_thermal_hwmon_get_temp(hs[i], s, &t, NULL) != 0)
                    break;
                char k[32];
                snprintf(k, sizeof(k), "temp%d", s);
                out_json_field_object_begin(&first, k);
                bool f2 = true;
                if (t.name[0])  out_json_field_string(&f2, "name", t.name);
                if (t.label[0]) out_json_field_string(&f2, "label", t.label);
                out_json_field_int(&f2, "temp_mc", t.temp);
                if (t.temp_max  >= 0) out_json_field_int(&f2, "max_mc", t.temp_max);
                if (t.temp_crit >= 0) out_json_field_int(&f2, "crit_mc", t.temp_crit);
                out_json_object_end();
            }
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"HWMON", "SENSOR", "NAME", "TEMP"};
        out_table_header(h, 4);
        for (int i = 0; i < n; i++) {
            for (int s = 1; s < 16; s++) {
                zenctl_hwmon_temp_t t;
                memset(&t, 0, sizeof(t));
                if (zenctl_thermal_hwmon_get_temp(hs[i], s, &t, NULL) != 0)
                    break;
                char idx[16]; snprintf(idx, sizeof(idx), "hwmon%d", hs[i]);
                char sensor[16]; snprintf(sensor, sizeof(sensor), "temp%d", s);
                char temp[32]; snprintf(temp, sizeof(temp), "%lld mC", (long long)t.temp);
                const char *r[] = {idx, sensor,
                                   t.name[0] ? t.name : "-", temp};
                out_table_row(r, 4);
            }
        }
    }
    return 0;
}

static int thermal_hwmon_fan(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 2)
        return cli_usage(json, "zenctl thermal hwmon fan <hwmon> <fan> [<0-255>]");
    long long hw, fan;
    if (cli_parse_int(argv[0], &hw) != 0 || hw < 0 ||
        cli_parse_int(argv[1], &fan) != 0 || fan < 1) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "hwmon and fan index must be non-negative integers",
                     "thermal hwmon fan");
        return cli_err(json, &err);
    }

    if (argc >= 3) {
        if (cli_require_root(json)) return -1;
        long long pwm;
        if (cli_parse_int(argv[2], &pwm) != 0 || pwm < 0 || pwm > 255) {
            zenctl_err_t err;
            cli_make_err(&err, ZENCTL_ERR_ERANGE,
                         "pwm must be 0-255", "thermal hwmon fan");
            return cli_err(json, &err);
        }
        if (dry_run) {
            char buf[64];
            snprintf(buf, sizeof(buf), "set hwmon%d fan%d pwm=%lld",
                     (int)hw, (int)fan, pwm);
            cli_dryrun(json, buf);
            return 0;
        }
        zenctl_err_t err;
        memset(&err, 0, sizeof(err));
        if (zenctl_thermal_hwmon_set_fan_pwm((int)hw, (int)fan, (int)pwm, &err) != 0)
            return cli_err(json, &err);
        if (json) {
            out_json_ok_begin();
            bool first = true;
            out_json_field_int(&first, "hwmon", hw);
            out_json_field_int(&first, "fan", fan);
            out_json_field_int(&first, "pwm", pwm);
            out_json_ok_end();
        } else {
            out_kv_int("hwmon", hw);
            out_kv_int("fan", fan);
            out_kv_int("set pwm", pwm);
        }
        return 0;
    }

    int pwm = -1; bool auto_mode = false;
    bool ok_pwm = (zenctl_thermal_hwmon_get_fan_pwm((int)hw, (int)fan, &pwm, NULL) == 0);
    bool ok_auto = (zenctl_thermal_hwmon_get_fan_auto((int)hw, (int)fan, &auto_mode, NULL) == 0);
    if (!ok_pwm && !ok_auto) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_ENOTSUP,
                     "no fan control on this hwmon/fan", "thermal hwmon fan");
        return cli_err(json, &err);
    }
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_int(&first, "hwmon", hw);
        out_json_field_int(&first, "fan", fan);
        if (ok_pwm)  out_json_field_int(&first, "pwm", pwm);
        if (ok_auto) out_json_field_bool(&first, "auto", auto_mode);
        out_json_ok_end();
    } else {
        out_kv_int("hwmon", hw);
        out_kv_int("fan", fan);
        if (ok_pwm)  out_kv_int("pwm", pwm);
        if (ok_auto) out_kv("auto", auto_mode ? "true" : "false");
    }
    return 0;
}

static int thermal_hwmon(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 1) return thermal_hwmon_list(json);
    if (strcmp(argv[0], "fan") == 0)
        return thermal_hwmon_fan(argc - 1, argv + 1, json, dry_run);
    return cli_usage(json, "zenctl thermal hwmon [fan <hwmon> <fan> [<0-255>]]");
}

/* ── thermal get ────────────────────────────────────────────────── */

static int thermal_get(int argc, char **argv, bool json)
{
    if (argc < 1)
        return cli_usage(json,
            "zenctl thermal get <temp|type|policy|mode|trips|cooling> --zone Z");
    const char *what = argv[0];
    const char *zn = cli_opt(argc, argv, "--zone");
    if (!zn) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "--zone <name> is required", "thermal get");
        return cli_err(json, &err);
    }

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_thermal_t *tz = zenctl_thermal_open(zn, &err);
    if (!tz) return cli_err(json, &err);

    int rc = 0;
    bool started_json = false;
    bool first = true;

    if (strcmp(what, "temp") == 0) {
        int64_t v = 0;
        if (zenctl_thermal_get_temp(tz, &v, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "zone", zn); out_json_field_int(&first, "temp_mc", v); out_json_field_double(&first, "temp_c", v / 1000.0); }
            else { out_kv("zone", zn); out_kv_int("temp_mc", v); char b[32]; snprintf(b, sizeof(b), "%.1f C", v / 1000.0); out_kv("temp", b); }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "type") == 0) {
        char *s = NULL;
        if (zenctl_thermal_get_type(tz, &s, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "zone", zn); out_json_field_string(&first, "type", s); }
            else { out_kv("zone", zn); out_kv("type", s); }
            free(s);
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "policy") == 0) {
        char *s = NULL;
        if (zenctl_thermal_get_policy(tz, &s, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "zone", zn); out_json_field_string(&first, "policy", s); }
            else { out_kv("zone", zn); out_kv("policy", s); }
            free(s);
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "mode") == 0) {
        bool enabled = false;
        if (zenctl_thermal_get_mode(tz, &enabled, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "zone", zn); out_json_field_bool(&first, "enabled", enabled); }
            else { out_kv("zone", zn); out_kv("enabled", enabled ? "true" : "false"); }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "trips") == 0) {
        int count = 0;
        if (zenctl_thermal_get_trip_count(tz, &count, &err) == 0) {
            if (json) {
                out_json_ok_begin(); started_json = true;
                out_json_field_string(&first, "zone", zn);
                out_json_field_array_begin(&first, "trips");
                for (int i = 0; i < count; i++) {
                    zenctl_trip_t trip;
                    memset(&trip, 0, sizeof(trip));
                    if (zenctl_thermal_get_trip(tz, i, &trip, NULL) != 0) continue;
                    if (i) fputc(',', stdout);
                    fputc('{', stdout);
                    bool f2 = true;
                    out_json_field_int(&f2, "index", trip.index);
                    out_json_field_string(&f2, "type", trip.type);
                    out_json_field_int(&f2, "temp_mc", trip.temp);
                    if (trip.hysteresis >= 0) out_json_field_int(&f2, "hyst_mc", trip.hysteresis);
                    fputc('}', stdout);
                }
                out_json_array_end();
            } else {
                out_kv("zone", zn);
                out_table_reset();
                const char *h[] = {"IDX", "TYPE", "TEMP", "HYST"};
                out_table_header(h, 4);
                for (int i = 0; i < count; i++) {
                    zenctl_trip_t trip;
                    memset(&trip, 0, sizeof(trip));
                    if (zenctl_thermal_get_trip(tz, i, &trip, NULL) != 0) continue;
                    char ib[16], tb[32], hb[32];
                    snprintf(ib, sizeof(ib), "%d", trip.index);
                    snprintf(tb, sizeof(tb), "%lld mC", (long long)trip.temp);
                    if (trip.hysteresis >= 0) snprintf(hb, sizeof(hb), "%lld mC", (long long)trip.hysteresis);
                    else snprintf(hb, sizeof(hb), "-");
                    const char *r[] = {ib, trip.type, tb, hb};
                    out_table_row(r, 4);
                }
            }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "cooling") == 0) {
        int count = 0;
        if (zenctl_thermal_get_cooling_count(tz, &count, &err) == 0) {
            if (json) {
                out_json_ok_begin(); started_json = true;
                out_json_field_string(&first, "zone", zn);
                out_json_field_array_begin(&first, "cooling");
                for (int i = 0; i < count; i++) {
                    zenctl_cooling_t c;
                    memset(&c, 0, sizeof(c));
                    if (zenctl_thermal_get_cooling(tz, i, &c, NULL) != 0) continue;
                    if (i) fputc(',', stdout);
                    fputc('{', stdout);
                    bool f2 = true;
                    out_json_field_int(&f2, "index", c.index);
                    out_json_field_string(&f2, "type", c.type);
                    out_json_field_int(&f2, "cur_state", c.cur_state);
                    out_json_field_int(&f2, "max_state", c.max_state);
                    fputc('}', stdout);
                }
                out_json_array_end();
            } else {
                out_kv("zone", zn);
                out_table_reset();
                const char *h[] = {"IDX", "TYPE", "CUR", "MAX"};
                out_table_header(h, 4);
                for (int i = 0; i < count; i++) {
                    zenctl_cooling_t c;
                    memset(&c, 0, sizeof(c));
                    if (zenctl_thermal_get_cooling(tz, i, &c, NULL) != 0) continue;
                    char ib[16], cb[16], mb[16];
                    snprintf(ib, sizeof(ib), "%d", c.index);
                    snprintf(cb, sizeof(cb), "%d", c.cur_state);
                    snprintf(mb, sizeof(mb), "%d", c.max_state);
                    const char *r[] = {ib, c.type, cb, mb};
                    out_table_row(r, 4);
                }
            }
        } else rc = cli_err(json, &err);
    } else {
        rc = cli_usage(json,
            "zenctl thermal get <temp|type|policy|mode|trips|cooling> --zone Z");
    }

    if (started_json) out_json_ok_end();
    zenctl_thermal_close(tz);
    return rc;
}

/* ── thermal set ────────────────────────────────────────────────── */

static int thermal_set(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 2)
        return cli_usage(json,
            "zenctl thermal set <policy|mode|cooling> <args> --zone Z");
    const char *what = argv[0];
    const char *zn = cli_opt(argc, argv, "--zone");
    if (!zn) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "--zone <name> is required", "thermal set");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_thermal_t *tz = zenctl_thermal_open(zn, &err);
    if (!tz) return cli_err(json, &err);

    int rc = 0;
    bool started_json = false;
    bool first = true;

    if (strcmp(what, "policy") == 0) {
        const char *val = argv[1];
        if (dry_run) { char b[128]; snprintf(b, sizeof(b), "set policy=%s zone=%s", val, zn); cli_dryrun(json, b); }
        else if (zenctl_thermal_set_policy(tz, val, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "zone", zn); out_json_field_string(&first, "policy", val); }
            else { out_kv("zone", zn); out_kv("set policy", val); }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "mode") == 0) {
        int b = cli_parse_bool(argv[1]);
        if (b < 0) {
            cli_make_err(&err, ZENCTL_ERR_EINVAL, "mode must be 'enabled' or 'disabled'", "thermal set mode");
            rc = cli_err(json, &err);
        } else if (dry_run) {
            char buf[128]; snprintf(buf, sizeof(buf), "set mode=%s zone=%s", b ? "enabled" : "disabled", zn);
            cli_dryrun(json, buf);
        } else if (zenctl_thermal_set_mode(tz, b ? true : false, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "zone", zn); out_json_field_bool(&first, "enabled", b ? true : false); }
            else { out_kv("zone", zn); out_kv("enabled", b ? "true" : "false"); }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "cooling") == 0) {
        if (argc < 3) {
            rc = cli_usage(json, "zenctl thermal set cooling <index> <state> --zone Z");
        } else {
            long long idx, state;
            if (cli_parse_int(argv[1], &idx) != 0 || idx < 0 ||
                cli_parse_int(argv[2], &state) != 0) {
                cli_make_err(&err, ZENCTL_ERR_EINVAL, "cooling index and state must be integers", "thermal set cooling");
                rc = cli_err(json, &err);
            } else if (dry_run) {
                char buf[128]; snprintf(buf, sizeof(buf), "set cooling[%lld]=%lld zone=%s", idx, state, zn);
                cli_dryrun(json, buf);
            } else if (zenctl_thermal_set_cooling_state(tz, (int)idx, (int)state, &err) == 0) {
                if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "zone", zn); out_json_field_int(&first, "cooling_index", idx); out_json_field_int(&first, "state", state); }
                else { out_kv("zone", zn); out_kv_int("cooling_index", idx); out_kv_int("state", state); }
            } else rc = cli_err(json, &err);
        }
    } else {
        rc = cli_usage(json, "zenctl thermal set <policy|mode|cooling> <args> --zone Z");
    }
    (void)confirm;
    if (started_json) out_json_ok_end();
    zenctl_thermal_close(tz);
    return rc;
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_thermal(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl thermal <list|get|set|hwmon> ...");
    if (strcmp(argv[0], "list")  == 0) return thermal_list(json);
    if (strcmp(argv[0], "hwmon") == 0) return thermal_hwmon(argc - 1, argv + 1, json, dry_run);
    if (strcmp(argv[0], "get")   == 0) return thermal_get(argc - 1, argv + 1, json);
    if (strcmp(argv[0], "set")   == 0) return thermal_set(argc - 1, argv + 1, json, dry_run, confirm);
    return cli_usage(json, "zenctl thermal <list|get|set|hwmon> ...");
}
