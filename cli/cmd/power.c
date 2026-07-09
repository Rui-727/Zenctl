/* power.c - zenctl power subcommand.
 *
 * Implements:
 *   zenctl power get {states|mem-sleep|battery[ N]|charge-thresholds N|ac|wakeup}
 *   zenctl power set {mem-sleep M|charge-start N P|charge-end N P}
 *   zenctl power suspend <freeze|mem|disk|standby> --confirm
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>

#include "zenctl/zenctl.h"
#include "zenctl/power.h"

#include "../output.h"
#include "common.h"

/* ── power get ──────────────────────────────────────────────────── */

static int power_get_states(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char *s = NULL;
    if (zenctl_power_get_supported_states(&s, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        fputs("\"states\":", stdout);
        fputc('[', stdout);
        bool first = true;
        char *save = NULL;
        for (char *t = strtok_r(s, " \t\n", &save); t;
             t = strtok_r(NULL, " \t\n", &save))
            out_json_array_string(&first, t);
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_kv("states", s);
    }
    free(s);
    return 0;
}

static int power_get_mem_sleep(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char *s = NULL;
    if (zenctl_power_get_mem_sleep_mode(&s, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "mem_sleep", s);
        out_json_ok_end();
    } else {
        out_kv("mem_sleep", s);
    }
    free(s);
    return 0;
}

static void battery_emit(bool json, bool *first, const zenctl_battery_t *b, int idx)
{
    if (json) {
        out_json_field_int(first, "index", idx);
        out_json_field_string(first, "name", b->name);
        if (b->status[0])     out_json_field_string(first, "status", b->status);
        if (b->capacity >= 0) out_json_field_int(first, "capacity", b->capacity);
        if (b->charge_full >= 0)        out_json_field_int(first, "charge_full_uah", b->charge_full);
        if (b->charge_full_design >= 0) out_json_field_int(first, "charge_full_design_uah", b->charge_full_design);
        if (b->charge_now >= 0)         out_json_field_int(first, "charge_now_uah", b->charge_now);
        if (b->current_now != 0)        out_json_field_int(first, "current_now_ua", b->current_now);
        if (b->voltage_now != 0)        out_json_field_int(first, "voltage_now_uv", b->voltage_now);
        if (b->cycle_count >= 0)        out_json_field_int(first, "cycle_count", b->cycle_count);
        if (b->technology[0])           out_json_field_string(first, "technology", b->technology);
    } else {
        out_kv("index", b->name);
        if (b->status[0])     out_kv("status", b->status);
        if (b->capacity >= 0) out_kv_int("capacity_percent", b->capacity);
        if (b->charge_full >= 0)        out_kv_int("charge_full_uah", b->charge_full);
        if (b->charge_full_design >= 0) out_kv_int("charge_full_design_uah", b->charge_full_design);
        if (b->charge_now >= 0)         out_kv_int("charge_now_uah", b->charge_now);
        if (b->current_now != 0)        out_kv_int("current_now_ua", b->current_now);
        if (b->voltage_now != 0)        out_kv_int("voltage_now_uv", b->voltage_now);
        if (b->cycle_count >= 0)        out_kv_int("cycle_count", b->cycle_count);
        if (b->technology[0])           out_kv("technology", b->technology);
    }
}

static int power_get_battery(int argc, char **argv, bool json)
{
    int idx = -1;
    if (argc >= 2) {
        long long v;
        if (cli_parse_int(argv[1], &v) != 0 || v < 0) {
            zenctl_err_t err;
            cli_make_err(&err, ZENCTL_ERR_EINVAL,
                         "battery index must be a non-negative integer",
                         "power get battery");
            return cli_err(json, &err);
        }
        idx = (int)v;
    }

    int count = 0;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    if (zenctl_power_battery_count(&count, &err) != 0)
        return cli_err(json, &err);

    if (idx >= 0) {
        zenctl_battery_t b;
        memset(&b, 0, sizeof(b));
        if (zenctl_power_battery_get(idx, &b, &err) != 0)
            return cli_err(json, &err);
        if (json) {
            out_json_ok_begin();
            fputc('{', stdout);
            bool first = true;
            battery_emit(json, &first, &b, idx);
            fputc('}', stdout);
            out_json_ok_end();
        } else {
            battery_emit(json, NULL, &b, idx);
        }
        return 0;
    }

    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < count; i++) {
            zenctl_battery_t b;
            memset(&b, 0, sizeof(b));
            if (zenctl_power_battery_get(i, &b, &err) != 0) continue;
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            battery_emit(json, &first, &b, i);
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"IDX", "NAME", "STATUS", "CAP"};
        out_table_header(h, 4);
        for (int i = 0; i < count; i++) {
            zenctl_battery_t b;
            memset(&b, 0, sizeof(b));
            if (zenctl_power_battery_get(i, &b, &err) != 0) continue;
            char ib[16], cb[16];
            snprintf(ib, sizeof(ib), "%d", i);
            snprintf(cb, sizeof(cb), "%d%%", b.capacity);
            const char *r[] = {ib, b.name,
                               b.status[0] ? b.status : "-", cb};
            out_table_row(r, 4);
        }
    }
    return 0;
}

static int power_get_charge_thresholds(int argc, char **argv, bool json)
{
    if (argc < 2) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "battery index required", "power get charge-thresholds");
        return cli_err(json, &err);
    }
    long long idx;
    if (cli_parse_int(argv[1], &idx) != 0 || idx < 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL, "bad index", "charge-thresholds");
        return cli_err(json, &err);
    }
    int start = -1, end = -1;
    zenctl_err_t e1, e2;
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));
    int r1 = zenctl_power_battery_get_charge_start((int)idx, &start, &e1);
    int r2 = zenctl_power_battery_get_charge_end((int)idx, &end, &e2);
    if (r1 != 0 && r2 != 0) {
        return cli_err(json, e1.code != 0 ? &e1 : &e2);
    }
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_int(&first, "index", idx);
        if (r1 == 0) out_json_field_int(&first, "start_percent", start);
        if (r2 == 0) out_json_field_int(&first, "end_percent", end);
        out_json_ok_end();
    } else {
        out_kv_int("index", idx);
        if (r1 == 0) out_kv_int("start_percent", start);
        if (r2 == 0) out_kv_int("end_percent", end);
    }
    return 0;
}

static int power_get_ac(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    bool online = false;
    if (zenctl_power_ac_online(&online, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_bool(&first, "online", online);
        out_json_ok_end();
    } else {
        out_kv("ac_online", online ? "true" : "false");
    }
    return 0;
}

static int power_get_wakeup(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char **list = NULL; int n = 0;
    if (zenctl_power_get_wakeup_devices(&list, &n, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            out_json_escape(list[i]);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        for (int i = 0; i < n; i++) puts(list[i]);
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    return 0;
}

static int power_get(int argc, char **argv, bool json)
{
    if (argc < 1)
        return cli_usage(json,
            "zenctl power get <states|mem-sleep|battery|charge-thresholds|ac|wakeup>");
    const char *what = argv[0];
    if (strcmp(what, "states") == 0)            return power_get_states(json);
    if (strcmp(what, "mem-sleep") == 0)         return power_get_mem_sleep(json);
    if (strcmp(what, "battery") == 0)           return power_get_battery(argc, argv, json);
    if (strcmp(what, "charge-thresholds") == 0) return power_get_charge_thresholds(argc, argv, json);
    if (strcmp(what, "ac") == 0)                return power_get_ac(json);
    if (strcmp(what, "wakeup") == 0)            return power_get_wakeup(json);
    return cli_usage(json,
        "zenctl power get <states|mem-sleep|battery|charge-thresholds|ac|wakeup>");
}

/* ── power set ──────────────────────────────────────────────────── */

static int power_set_mem_sleep(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 2) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "mem_sleep mode required", "power set mem-sleep");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char b[128]; snprintf(b, sizeof(b), "set mem_sleep=%s", argv[1]);
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    if (zenctl_power_set_mem_sleep_mode(argv[1], &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "mem_sleep", argv[1]);
        out_json_ok_end();
    } else {
        out_kv("set mem_sleep", argv[1]);
    }
    return 0;
}

static int power_set_charge(int argc, char **argv, bool json, bool dry_run, int is_start)
{
    if (argc < 3) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "battery index and percent required",
                     "power set charge-*");
        return cli_err(json, &err);
    }
    long long idx, pct;
    if (cli_parse_int(argv[1], &idx) != 0 || idx < 0 ||
        cli_parse_int(argv[2], &pct) != 0 || pct < 0 || pct > 100) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "bad index or percent (0..100)", "power set charge-*");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char b[128];
        snprintf(b, sizeof(b), "set %s battery[%lld]=%lld%%",
                 is_start ? "charge_start" : "charge_end", idx, pct);
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = is_start
        ? zenctl_power_battery_set_charge_start((int)idx, (int)pct, &err)
        : zenctl_power_battery_set_charge_end((int)idx, (int)pct, &err);
    if (rc != 0) return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_int(&first, "index", idx);
        out_json_field_int(&first, is_start ? "start_percent" : "end_percent", pct);
        out_json_ok_end();
    } else {
        out_kv_int("index", idx);
        out_kv_int(is_start ? "set start_percent" : "set end_percent", pct);
    }
    return 0;
}

static int power_set(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 1)
        return cli_usage(json, "zenctl power set <mem-sleep|charge-start|charge-end> ...");
    const char *what = argv[0];
    if (strcmp(what, "mem-sleep") == 0)     return power_set_mem_sleep(argc, argv, json, dry_run);
    if (strcmp(what, "charge-start") == 0)  return power_set_charge(argc, argv, json, dry_run, 1);
    if (strcmp(what, "charge-end") == 0)    return power_set_charge(argc, argv, json, dry_run, 0);
    return cli_usage(json, "zenctl power set <mem-sleep|charge-start|charge-end> ...");
}

/* ── power suspend ──────────────────────────────────────────────── */

static int power_suspend(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl power suspend <freeze|mem|disk|standby> --confirm");
    const char *name = argv[0];
    zenctl_sleep_state_t state;
    if      (strcmp(name, "freeze")  == 0) state = ZENCTL_SLEEP_FREEZE;
    else if (strcmp(name, "mem")     == 0) state = ZENCTL_SLEEP_MEM;
    else if (strcmp(name, "disk")    == 0) state = ZENCTL_SLEEP_DISK;
    else if (strcmp(name, "standby") == 0) state = ZENCTL_SLEEP_STANDBY;
    else {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "state must be freeze|mem|disk|standby",
                     "power suspend");
        return cli_err(json, &err);
    }

    /* Suspend is destructive (freezes the system). Require --confirm. */
    if (!confirm) {
        if (json) out_json_error(ZENCTL_ERR_EPERM,
                       "suspend requires --confirm to proceed");
        else out_err_code(ZENCTL_ERR_EPERM,
                       "suspend requires --confirm to proceed");
        return -1;
    }
    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char b[64]; snprintf(b, sizeof(b), "would suspend to %s", name);
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    if (zenctl_power_suspend(state, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "resumed_from", name);
        out_json_ok_end();
    } else {
        out_kv("resumed_from", name);
    }
    return 0;
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_power(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl power <get|set|suspend> ...");
    if (strcmp(argv[0], "get")     == 0) return power_get(argc - 1, argv + 1, json);
    if (strcmp(argv[0], "set")     == 0) return power_set(argc - 1, argv + 1, json, dry_run);
    if (strcmp(argv[0], "suspend") == 0) return power_suspend(argc - 1, argv + 1, json, dry_run, confirm);
    return cli_usage(json, "zenctl power <get|set|suspend> ...");
}
