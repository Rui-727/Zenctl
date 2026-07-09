/* wireless.c - zenctl wireless subcommand.
 *
 * Implements:
 *   zenctl wireless list
 *   zenctl wireless get {power|regdomain} <phy>
 *   zenctl wireless set power <phy> <on|off>
 *
 * `<phy>` is a cfg80211 PHY name like "phy0".
 *
 * regdomain set and TX-power set are not implemented in libzenctl v1
 * (they require nl80211); `wireless get regdomain` returns ENOTSUP
 * from the library and we surface that as an error.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/wireless.h"

#include "../output.h"
#include "common.h"

/* ── wireless list ──────────────────────────────────────────────── */

static int wireless_list(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char **list = NULL; int n = 0;
    if (zenctl_wireless_enumerate(&list, &n, &err) != 0)
        return cli_err(json, &err);

    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            out_json_field_string(&first, "phy", list[i]);
            zenctl_wireless_t *wl = zenctl_wireless_open(list[i], NULL);
            if (wl) {
                bool blocked = false;
                if (zenctl_wireless_get_rfkill_blocked(wl, &blocked, NULL) == 0)
                    out_json_field_bool(&first, "powered", !blocked);
                char *rd = NULL;
                if (zenctl_wireless_get_regdomain(wl, &rd, NULL) == 0) {
                    out_json_field_string(&first, "regdomain", rd);
                    free(rd);
                }
                zenctl_wireless_close(wl);
            }
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"PHY", "POWERED", "REGDOMAIN"};
        out_table_header(h, 3);
        for (int i = 0; i < n; i++) {
            zenctl_wireless_t *wl = zenctl_wireless_open(list[i], NULL);
            bool blocked = true; char *rd = NULL;
            if (wl) {
                zenctl_wireless_get_rfkill_blocked(wl, &blocked, NULL);
                zenctl_wireless_get_regdomain(wl, &rd, NULL);
            }
            const char *r[] = {list[i], blocked ? "off" : "on",
                               rd ? rd : "-"};
            out_table_row(r, 3);
            free(rd);
            if (wl) zenctl_wireless_close(wl);
        }
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    return 0;
}

/* ── wireless get ───────────────────────────────────────────────── */

static int wireless_get(int argc, char **argv, bool json)
{
    if (argc < 2)
        return cli_usage(json, "zenctl wireless get <power|regdomain> <phy>");
    const char *what = argv[0];
    const char *phy = argv[1];

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_wireless_t *wl = zenctl_wireless_open(phy, &err);
    if (!wl) return cli_err(json, &err);

    int rc = 0;
    bool started_json = false;
    bool first = true;

    if (strcmp(what, "power") == 0) {
        bool blocked = false;
        if (zenctl_wireless_get_rfkill_blocked(wl, &blocked, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "phy", phy); out_json_field_bool(&first, "powered", !blocked); }
            else out_kv("powered", blocked ? "off" : "on");
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "regdomain") == 0) {
        char *s = NULL;
        if (zenctl_wireless_get_regdomain(wl, &s, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_string(&first, "phy", phy); out_json_field_string(&first, "regdomain", s); }
            else out_kv("regdomain", s);
            free(s);
        } else rc = cli_err(json, &err);
    } else {
        rc = cli_usage(json, "zenctl wireless get <power|regdomain> <phy>");
    }

    if (started_json) out_json_ok_end();
    zenctl_wireless_close(wl);
    return rc;
}

/* ── wireless set ───────────────────────────────────────────────── */

static int wireless_set(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 3)
        return cli_usage(json, "zenctl wireless set power <phy> <on|off>");
    if (strcmp(argv[0], "power") != 0)
        return cli_usage(json, "zenctl wireless set power <phy> <on|off>");
    const char *phy = argv[1];
    int b = cli_parse_bool(argv[2]);
    if (b < 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EINVAL,
                     "value must be 'on' or 'off'", "wireless set power");
        return cli_err(json, &err);
    }
    if (cli_require_root(json)) return -1;

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_wireless_t *wl = zenctl_wireless_open(phy, &err);
    if (!wl) return cli_err(json, &err);

    int rc = 0;
    if (dry_run) {
        char buf[128];
        snprintf(buf, sizeof(buf), "set power=%s phy=%s",
                 b ? "on" : "off", phy);
        cli_dryrun(json, buf);
    } else if (zenctl_wireless_set_rfkill_blocked(wl, b ? false : true, &err) == 0) {
        if (json) { out_json_ok_begin(); bool first = true; out_json_field_string(&first, "phy", phy); out_json_field_bool(&first, "powered", b ? true : false); out_json_ok_end(); }
        else { out_kv("phy", phy); out_kv("set powered", b ? "on" : "off"); }
    } else rc = cli_err(json, &err);
    zenctl_wireless_close(wl);
    return rc;
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_wireless(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)confirm;
    if (argc < 1)
        return cli_usage(json, "zenctl wireless <list|get|set> ...");
    if (strcmp(argv[0], "list") == 0) return wireless_list(json);
    if (strcmp(argv[0], "get")  == 0) return wireless_get(argc - 1, argv + 1, json);
    if (strcmp(argv[0], "set")  == 0) return wireless_set(argc - 1, argv + 1, json, dry_run);
    return cli_usage(json, "zenctl wireless <list|get|set> ...");
}
