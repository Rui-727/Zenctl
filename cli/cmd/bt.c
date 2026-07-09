/* bt.c - zenctl bt subcommand.
 *
 * Implements:
 *   zenctl bt list
 *   zenctl bt get {power|address} <adapter>
 *   zenctl bt set power <adapter> <on|off>
 *
 * `<adapter>` is an hciN name like "hci0".
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/bt.h"

#include "../output.h"
#include "common.h"

/* ── bt list ────────────────────────────────────────────────────── */

static int bt_list(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char **list = NULL; int n = 0;
    if (zenctl_bt_enumerate(&list, &n, &err) != 0)
        return cli_err(json, &err);

    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            out_json_field_string(&first, "adapter", list[i]);
            zenctl_bt_t *bt = zenctl_bt_open(list[i], NULL);
            if (bt) {
                bool on = false;
                if (zenctl_bt_get_powered(bt, &on, NULL) == 0)
                    out_json_field_bool(&first, "powered", on);
                char *addr = NULL;
                if (zenctl_bt_get_address(bt, &addr, NULL) == 0) {
                    out_json_field_string(&first, "address", addr);
                    free(addr);
                }
                zenctl_bt_close(bt);
            }
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"ADAPTER", "POWERED", "ADDRESS"};
        out_table_header(h, 3);
        for (int i = 0; i < n; i++) {
            zenctl_bt_t *bt = zenctl_bt_open(list[i], NULL);
            bool on = false; char *addr = NULL;
            if (bt) {
                zenctl_bt_get_powered(bt, &on, NULL);
                zenctl_bt_get_address(bt, &addr, NULL);
            }
            const char *r[] = {list[i], on ? "on" : "off",
                               addr ? addr : "-"};
            out_table_row(r, 3);
            free(addr);
            if (bt) zenctl_bt_close(bt);
        }
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    return 0;
}

/* ── bt get ─────────────────────────────────────────────────────── */

static int bt_get(int argc, char **argv, bool json)
{
    if (argc < 2)
        return cli_usage(json, "zenctl bt get <power|address> <adapter>");
    const char *what = argv[0];
    const char *adapter = argv[1];

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_bt_t *bt = zenctl_bt_open(adapter, &err);
    if (!bt) return cli_err(json, &err);

    int rc = 0;
    bool started_json = false;
    bool first = true;

    if (strcmp(what, "power") == 0) {
        bool on = false;
        if (zenctl_bt_get_powered(bt, &on, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "adapter", adapter); out_json_field_bool(&first, "powered", on); }
            else out_kv("powered", on ? "on" : "off");
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "address") == 0) {
        char *s = NULL;
        if (zenctl_bt_get_address(bt, &s, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "adapter", adapter); out_json_field_string(&first, "address", s); }
            else out_kv("address", s);
            free(s);
        } else rc = cli_err(json, &err);
    } else {
        rc = cli_usage(json, "zenctl bt get <power|address> <adapter>");
    }

    if (started_json) out_json_ok_end();
    zenctl_bt_close(bt);
    return rc;
}

/* ── bt set ─────────────────────────────────────────────────────── */

static int bt_set(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 3)
        return cli_usage(json, "zenctl bt set power <adapter> <on|off>");
    if (strcmp(argv[0], "power") != 0)
        return cli_usage(json, "zenctl bt set power <adapter> <on|off>");
    const char *adapter = argv[1];
    int b = cli_parse_bool(argv[2]);
    if (b < 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "value must be 'on' or 'off'", "bt set power");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_bt_t *bt = zenctl_bt_open(adapter, &err);
    if (!bt) return cli_err(json, &err);

    int rc = 0;
    if (dry_run) {
        char buf[128];
        snprintf(buf, sizeof(buf), "set power=%s adapter=%s",
                 b ? "on" : "off", adapter);
        cli_dryrun(json, buf);
    } else if (zenctl_bt_set_powered(bt, b ? true : false, &err) == 0) {
        if (json) { out_json_ok_begin(); bool first = true; out_json_field_string(&first, "adapter", adapter); out_json_field_bool(&first, "powered", b ? true : false); out_json_ok_end(); }
        else { out_kv("adapter", adapter); out_kv("set powered", b ? "on" : "off"); }
    } else rc = cli_err(json, &err);
    zenctl_bt_close(bt);
    return rc;
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_bt(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)confirm;
    if (argc < 1)
        return cli_usage(json, "zenctl bt <list|get|set> ...");
    if (strcmp(argv[0], "list") == 0) return bt_list(json);
    if (strcmp(argv[0], "get")  == 0) return bt_get(argc - 1, argv + 1, json);
    if (strcmp(argv[0], "set")  == 0) return bt_set(argc - 1, argv + 1, json, dry_run);
    return cli_usage(json, "zenctl bt <list|get|set> ...");
}
