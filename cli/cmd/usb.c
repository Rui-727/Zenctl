/* usb.c - zenctl usb subcommand.
 *
 * Implements:
 *   zenctl usb list
 *   zenctl usb get {power|autosuspend|authorized|wakeup} <dev>
 *   zenctl usb set {power|autosuspend|authorized|wakeup} <dev> <val>
 *   zenctl usb reset <dev>
 *
 * `<dev>` is a USB device path like "1-2" or "2-1.3" (the directory
 * name under /sys/bus/usb/devices/).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/usb.h"

#include "../output.h"
#include "common.h"

static zenctl_usb_t *usb_open(bool json, const char *dev, zenctl_err_t *err)
{
    zenctl_usb_t *u = zenctl_usb_open(dev, err);
    if (!u) cli_err(json, err);
    return u;
}

/* ── usb list ───────────────────────────────────────────────────── */

static int usb_list(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char **list = NULL; int n = 0;
    if (zenctl_usb_enumerate(&list, &n, &err) != 0)
        return cli_err(json, &err);

    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            out_json_field_string(&first, "dev", list[i]);
            zenctl_usb_t *u = zenctl_usb_open(list[i], NULL);
            if (u) {
                int vid = 0, pid = 0;
                char *s = NULL;
                if (zenctl_usb_get_vendor_id(u, &vid, NULL) == 0)
                    out_json_field_int(&first, "idVendor", vid);
                if (zenctl_usb_get_product_id(u, &pid, NULL) == 0)
                    out_json_field_int(&first, "idProduct", pid);
                if (zenctl_usb_get_manufacturer(u, &s, NULL) == 0) {
                    out_json_field_string(&first, "manufacturer", s); free(s);
                }
                if (zenctl_usb_get_product(u, &s, NULL) == 0) {
                    out_json_field_string(&first, "product", s); free(s);
                }
                if (zenctl_usb_get_speed(u, &s, NULL) == 0) {
                    out_json_field_string(&first, "speed", s); free(s);
                }
                zenctl_usb_close(u);
            }
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"DEV", "VID", "PID", "PRODUCT", "SPEED"};
        out_table_header(h, 5);
        for (int i = 0; i < n; i++) {
            zenctl_usb_t *u = zenctl_usb_open(list[i], NULL);
            char vidb[8] = "-", pidb[8] = "-";
            char *prod = NULL, *speed = NULL;
            int vid = 0, pid = 0;
            if (u) {
                if (zenctl_usb_get_vendor_id(u, &vid, NULL) == 0)
                    snprintf(vidb, sizeof(vidb), "%04x", vid);
                if (zenctl_usb_get_product_id(u, &pid, NULL) == 0)
                    snprintf(pidb, sizeof(pidb), "%04x", pid);
                zenctl_usb_get_product(u, &prod, NULL);
                zenctl_usb_get_speed(u, &speed, NULL);
            }
            const char *r[] = {list[i], vidb, pidb,
                               prod ? prod : "-", speed ? speed : "-"};
            out_table_row(r, 5);
            free(prod); free(speed);
            if (u) zenctl_usb_close(u);
        }
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    return 0;
}

/* ── usb get ────────────────────────────────────────────────────── */

static int usb_get(int argc, char **argv, bool json)
{
    if (argc < 2)
        return cli_usage(json, "zenctl usb get <power|autosuspend|authorized|wakeup> <dev>");
    const char *what = argv[0];
    const char *dev = argv[1];

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *u = usb_open(json, dev, &err);
    if (!u) return -1;

    int rc = 0;
    bool started_json = false;
    bool first = true;

    if (strcmp(what, "power") == 0) {
        char *s = NULL;
        if (zenctl_usb_get_power_control(u, &s, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "dev", dev); out_json_field_string(&first, "control", s); }
            else out_kv("power_control", s);
            free(s);
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "autosuspend") == 0) {
        int v = 0;
        if (zenctl_usb_get_autosuspend_delay(u, &v, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "dev", dev); out_json_field_int(&first, "delay_ms", v); }
            else out_kv_int("autosuspend_delay_ms", v);
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "authorized") == 0) {
        bool b = false;
        if (zenctl_usb_get_authorized(u, &b, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "dev", dev); out_json_field_bool(&first, "authorized", b); }
            else out_kv("authorized", b ? "true" : "false");
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "wakeup") == 0) {
        bool b = false;
        if (zenctl_usb_get_wakeup(u, &b, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "dev", dev); out_json_field_bool(&first, "wakeup", b); }
            else out_kv("wakeup", b ? "enabled" : "disabled");
        } else rc = cli_err(json, &err);
    } else {
        rc = cli_usage(json, "zenctl usb get <power|autosuspend|authorized|wakeup> <dev>");
    }

    if (started_json) out_json_ok_end();
    zenctl_usb_close(u);
    return rc;
}

/* ── usb set ────────────────────────────────────────────────────── */

static int usb_set_power(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 3)
        return cli_usage(json, "zenctl usb set power <auto|on> <dev>");
    const char *mode = argv[1];
    const char *dev  = argv[2];
    if (strcmp(mode, "auto") != 0 && strcmp(mode, "on") != 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "mode must be 'auto' or 'on'", "usb set power");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char b[128]; snprintf(b, sizeof(b), "set power=%s dev=%s", mode, dev);
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *u = usb_open(json, dev, &err);
    if (!u) return -1;
    int rc = 0;
    if (zenctl_usb_set_power_control(u, mode, &err) == 0) {
        if (json) { out_json_ok_begin(); bool first = true; out_json_field_string(&first, "dev", dev); out_json_field_string(&first, "control", mode); out_json_ok_end(); }
        else { out_kv("dev", dev); out_kv("set power", mode); }
    } else rc = cli_err(json, &err);
    zenctl_usb_close(u);
    return rc;
}

static int usb_set_autosuspend(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 3)
        return cli_usage(json, "zenctl usb set autosuspend <dev> <ms>");
    const char *dev = argv[1];
    long long ms;
    if (cli_parse_int(argv[2], &ms) != 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL, "ms must be an integer",
                     "usb set autosuspend");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char b[128]; snprintf(b, sizeof(b), "set autosuspend dev=%s ms=%lld", dev, ms);
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *u = usb_open(json, dev, &err);
    if (!u) return -1;
    int rc = 0;
    if (zenctl_usb_set_autosuspend_delay(u, (int)ms, &err) == 0) {
        if (json) { out_json_ok_begin(); bool first = true; out_json_field_string(&first, "dev", dev); out_json_field_int(&first, "delay_ms", ms); out_json_ok_end(); }
        else { out_kv("dev", dev); out_kv_int("set delay_ms", ms); }
    } else rc = cli_err(json, &err);
    zenctl_usb_close(u);
    return rc;
}

static int usb_set_authorized(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 3)
        return cli_usage(json, "zenctl usb set authorized <dev> <on|off>");
    const char *dev = argv[1];
    int b = cli_parse_bool(argv[2]);
    if (b < 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "value must be 'on' or 'off'", "usb set authorized");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;
    if (!b && !confirm) {
        if (json) out_json_error(ZENCTL_ERR_EPERM,
                       "de-authorizing a device requires --confirm");
        else out_err_code(ZENCTL_ERR_EPERM,
                       "de-authorizing a device requires --confirm");
        return -1;
    }
    if (dry_run) {
        char buf[128]; snprintf(buf, sizeof(buf), "set authorized=%s dev=%s",
                 b ? "on" : "off", dev);
        cli_dryrun(json, buf);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *u = usb_open(json, dev, &err);
    if (!u) return -1;
    int rc = 0;
    if (zenctl_usb_set_authorized(u, b ? true : false, &err) == 0) {
        if (json) { out_json_ok_begin(); bool first = true; out_json_field_string(&first, "dev", dev); out_json_field_bool(&first, "authorized", b ? true : false); out_json_ok_end(); }
        else { out_kv("dev", dev); out_kv("set authorized", b ? "on" : "off"); }
    } else rc = cli_err(json, &err);
    zenctl_usb_close(u);
    return rc;
}

static int usb_set_wakeup(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 3)
        return cli_usage(json, "zenctl usb set wakeup <dev> <on|off>");
    const char *dev = argv[1];
    int b = cli_parse_bool(argv[2]);
    if (b < 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "value must be 'on' or 'off'", "usb set wakeup");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char buf[128]; snprintf(buf, sizeof(buf), "set wakeup=%s dev=%s",
                 b ? "on" : "off", dev);
        cli_dryrun(json, buf);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *u = usb_open(json, dev, &err);
    if (!u) return -1;
    int rc = 0;
    if (zenctl_usb_set_wakeup(u, b ? true : false, &err) == 0) {
        if (json) { out_json_ok_begin(); bool first = true; out_json_field_string(&first, "dev", dev); out_json_field_bool(&first, "wakeup", b ? true : false); out_json_ok_end(); }
        else { out_kv("dev", dev); out_kv("set wakeup", b ? "enabled" : "disabled"); }
    } else rc = cli_err(json, &err);
    zenctl_usb_close(u);
    return rc;
}

static int usb_set(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl usb set <power|autosuspend|authorized|wakeup> ...");
    const char *what = argv[0];
    if (strcmp(what, "power") == 0)        return usb_set_power(argc, argv, json, dry_run);
    if (strcmp(what, "autosuspend") == 0)  return usb_set_autosuspend(argc, argv, json, dry_run);
    if (strcmp(what, "authorized") == 0)   return usb_set_authorized(argc, argv, json, dry_run, confirm);
    if (strcmp(what, "wakeup") == 0)       return usb_set_wakeup(argc, argv, json, dry_run);
    return cli_usage(json, "zenctl usb set <power|autosuspend|authorized|wakeup> ...");
}

/* ── usb reset ──────────────────────────────────────────────────── */

static int usb_reset(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 1)
        return cli_usage(json, "zenctl usb reset <dev>");
    const char *dev = argv[0];
    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char b[64]; snprintf(b, sizeof(b), "reset dev=%s", dev);
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *u = usb_open(json, dev, &err);
    if (!u) return -1;
    int rc = 0;
    if (zenctl_usb_reset(u, &err) == 0) {
        if (json) { out_json_ok_begin(); bool first = true; out_json_field_string(&first, "dev", dev); out_json_field_bool(&first, "reset", true); out_json_ok_end(); }
        else { out_kv("dev", dev); out_kv("reset", "ok"); }
    } else rc = cli_err(json, &err);
    zenctl_usb_close(u);
    return rc;
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_usb(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl usb <list|get|set|reset> ...");
    if (strcmp(argv[0], "list")  == 0) return usb_list(json);
    if (strcmp(argv[0], "get")   == 0) return usb_get(argc - 1, argv + 1, json);
    if (strcmp(argv[0], "set")   == 0) return usb_set(argc - 1, argv + 1, json, dry_run, confirm);
    if (strcmp(argv[0], "reset") == 0) return usb_reset(argc - 1, argv + 1, json, dry_run);
    return cli_usage(json, "zenctl usb <list|get|set|reset> ...");
}
