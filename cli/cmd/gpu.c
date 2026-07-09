/* gpu.c - zenctl gpu subcommand.
 *
 * Implements the GPU commands listed in the task spec:
 *   zenctl gpu list
 *   zenctl gpu get {temp|fan|power|freq|driver|profile|busy|--all} [--card N]
 *   zenctl gpu set fan {<0-255>|auto}            [--card N]
 *   zenctl gpu set profile <name>                [--card N]
 *
 * Card enumeration scans /sys/class/drm for "cardN" entries (pure-digit
 * suffix — "card0-render" etc. are skipped). Per-card state is loaded
 * through the libzenctl gpu API (typed handle, hwmon-backed reads,
 * amdgpu-specific profile/busy paths).
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
#include "zenctl/gpu.h"

#include "../output.h"
#include "common.h"

/* ── Card enumeration ───────────────────────────────────────────── */

/* Collect card indices from /sys/class/drm/. Returns 0 on success and
 * fills `out` (caller-allocated, capacity `cap`) with sorted indices. */
static int enum_cards(int *out, int cap, int *count)
{
    *count = 0;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "card", 4) != 0) continue;
        const char *p = de->d_name + 4;
        if (!isdigit((unsigned char)*p)) continue;
        bool pure = true;
        for (; *p; p++) if (!isdigit((unsigned char)*p)) { pure = false; break; }
        if (!pure) continue;
        if (*count >= cap) break;
        out[*count] = atoi(de->d_name + 4);
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

/* Open a card by index; on failure emit the right error and return NULL. */
static zenctl_gpu_t *open_card(bool json, int card, zenctl_err_t *err)
{
    zenctl_gpu_t *g = zenctl_gpu_open(card, err);
    if (!g) cli_err(json, err);
    return g;
}

/* ── gpu list ───────────────────────────────────────────────────── */

static int gpu_list(bool json)
{
    int cards[64], n = 0;
    if (enum_cards(cards, 64, &n) != 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EIO,
                     "cannot read /sys/class/drm", "gpu_list");
        return cli_err(json, &err);
    }
    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            out_json_field_int(&first, "card", cards[i]);
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            zenctl_gpu_t *g = zenctl_gpu_open(cards[i], &err);
            if (g) {
                char *s = NULL;
                if (zenctl_gpu_get_driver(g, &s, NULL) == 0) {
                    out_json_field_string(&first, "driver", s);
                    free(s);
                }
                if (zenctl_gpu_get_vendor(g, &s, NULL) == 0) {
                    out_json_field_string(&first, "vendor", s);
                    free(s);
                }
                zenctl_gpu_close(g);
            }
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"CARD", "DRIVER", "VENDOR"};
        out_table_header(h, 3);
        for (int i = 0; i < n; i++) {
            char cn[16];
            snprintf(cn, sizeof(cn), "%d", cards[i]);
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            zenctl_gpu_t *g = zenctl_gpu_open(cards[i], &err);
            char *drv = NULL, *vendor = NULL;
            if (g) {
                zenctl_gpu_get_driver(g, &drv, NULL);
                zenctl_gpu_get_vendor(g, &vendor, NULL);
            }
            const char *r[] = {cn, drv ? drv : "-",
                               vendor ? vendor : "-"};
            out_table_row(r, 3);
            free(drv); free(vendor);
            if (g) zenctl_gpu_close(g);
        }
    }
    return 0;
}

/* ── gpu get ────────────────────────────────────────────────────── */

/* Emit one GPU's full state as a JSON object body (no surrounding {}).
 * Used by `gpu get --all`. */
static void gpu_emit_all_json(zenctl_gpu_t *g, bool *first)
{
    int64_t i64; char *s; int ival; bool bval;

    if (zenctl_gpu_get_driver(g, &s, NULL) == 0) {
        out_json_field_string(first, "driver", s); free(s);
    }
    if (zenctl_gpu_get_vendor(g, &s, NULL) == 0) {
        out_json_field_string(first, "vendor", s); free(s);
    }
    if (zenctl_gpu_get_temp(g, &i64, NULL) == 0)
        out_json_field_int(first, "temp_mc", i64);
    if (zenctl_gpu_get_power(g, &i64, NULL) == 0)
        out_json_field_int(first, "power_uw", i64);
    if (zenctl_gpu_get_freq(g, &i64, NULL) == 0)
        out_json_field_int(first, "freq_mhz", i64);
    if (zenctl_gpu_get_fan_pwm(g, &ival, NULL) == 0)
        out_json_field_int(first, "fan_pwm", ival);
    if (zenctl_gpu_get_fan_auto(g, &bval, NULL) == 0)
        out_json_field_bool(first, "fan_auto", bval);
    if (zenctl_gpu_amdgpu_get_power_profile(g, &s, NULL) == 0) {
        out_json_field_string(first, "profile", s); free(s);
    }
    if (zenctl_gpu_amdgpu_get_busy_percent(g, &ival, NULL) == 0)
        out_json_field_int(first, "busy_percent", ival);
}

/* `gpu get --all` prints every readable attribute of every card (or
 * the one selected by --card). */
static int gpu_get_all(bool json, int argc, char **argv)
{
    int sel = cli_opt_int(argc, argv, "--card", -1);
    int cards[64], n = 0;
    if (enum_cards(cards, 64, &n) != 0) {
        zenctl_err_t err;
        cli_make_err(&err, ZENCTL_ERR_EIO,
                     "cannot read /sys/class/drm", "gpu_get_all");
        return cli_err(json, &err);
    }
    if (sel >= 0) { cards[0] = sel; n = 1; }

    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            zenctl_gpu_t *g = zenctl_gpu_open(cards[i], &err);
            bool first = true;
            out_json_field_int(&first, "card", cards[i]);
            if (g) {
                gpu_emit_all_json(g, &first);
                zenctl_gpu_close(g);
            }
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        for (int i = 0; i < n; i++) {
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            zenctl_gpu_t *g = zenctl_gpu_open(cards[i], &err);
            if (!g) { cli_err(json, &err); continue; }
            printf("card %d:\n", cards[i]);
            char *s; int64_t i64; int ival; bool bval;
            if (zenctl_gpu_get_driver(g, &s, NULL) == 0) { out_kv("driver", s); free(s); }
            if (zenctl_gpu_get_vendor(g, &s, NULL) == 0) { out_kv("vendor", s); free(s); }
            if (zenctl_gpu_get_temp(g, &i64, NULL) == 0)    out_kv_int("temp_mc", i64);
            if (zenctl_gpu_get_power(g, &i64, NULL) == 0)   out_kv_int("power_uw", i64);
            if (zenctl_gpu_get_freq(g, &i64, NULL) == 0)    out_kv_int("freq_mhz", i64);
            if (zenctl_gpu_get_fan_pwm(g, &ival, NULL) == 0) out_kv_int("fan_pwm", ival);
            if (zenctl_gpu_get_fan_auto(g, &bval, NULL) == 0) out_kv("fan_auto", bval ? "true" : "false");
            if (zenctl_gpu_amdgpu_get_power_profile(g, &s, NULL) == 0) { out_kv("profile", s); free(s); }
            if (zenctl_gpu_amdgpu_get_busy_percent(g, &ival, NULL) == 0) out_kv_int("busy_percent", ival);
            zenctl_gpu_close(g);
        }
    }
    return 0;
}

static int gpu_get(int argc, char **argv, bool json)
{
    if (argc < 1)
        return cli_usage(json, "zenctl gpu get <temp|fan|power|freq|driver|profile|busy|--all> [--card N]");

    if (strcmp(argv[0], "--all") == 0)
        return gpu_get_all(json, argc, argv);

    const char *what = argv[0];
    int card = cli_opt_int(argc, argv, "--card", 0);

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *g = open_card(json, card, &err);
    if (!g) return -1;

    int rc = 0;
    bool started_json = false;
    bool first = true;

    if (strcmp(what, "temp") == 0) {
        int64_t v = 0;
        if (zenctl_gpu_get_temp(g, &v, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_int(&first, "card", card); out_json_field_int(&first, "temp_mc", v); out_json_field_double(&first, "temp_c", v / 1000.0); }
            else { out_kv_int("card", card); out_kv_int("temp_mc", v); char b[32]; snprintf(b, sizeof(b), "%.1f C", v / 1000.0); out_kv("temp", b); }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "fan") == 0) {
        int pwm = -1; bool auto_mode = false;
        bool ok_pwm = (zenctl_gpu_get_fan_pwm(g, &pwm, NULL) == 0);
        bool ok_auto = (zenctl_gpu_get_fan_auto(g, &auto_mode, NULL) == 0);
        if (ok_pwm || ok_auto) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_int(&first, "card", card); if (ok_pwm) out_json_field_int(&first, "pwm", pwm); if (ok_auto) out_json_field_bool(&first, "auto", auto_mode); }
            else { out_kv_int("card", card); if (ok_pwm) out_kv_int("pwm", pwm); if (ok_auto) out_kv("auto", auto_mode ? "true" : "false"); }
        } else {
            cli_make_err(&err, ZENCTL_ERR_ENOTSUP, "no fan control exposed by driver", "gpu get fan");
            rc = cli_err(json, &err);
        }
    } else if (strcmp(what, "power") == 0) {
        int64_t v = 0;
        if (zenctl_gpu_get_power(g, &v, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_int(&first, "card", card); out_json_field_int(&first, "power_uw", v); out_json_field_double(&first, "power_w", v / 1000000.0); }
            else { out_kv_int("card", card); out_kv_int("power_uw", v); char b[32]; snprintf(b, sizeof(b), "%.2f W", v / 1000000.0); out_kv("power", b); }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "freq") == 0) {
        int64_t v = 0;
        if (zenctl_gpu_get_freq(g, &v, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_int(&first, "card", card); out_json_field_int(&first, "freq_mhz", v); }
            else { out_kv_int("card", card); out_kv_int("freq_mhz", v); }
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "driver") == 0) {
        char *s = NULL;
        if (zenctl_gpu_get_driver(g, &s, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_int(&first, "card", card); out_json_field_string(&first, "driver", s); }
            else { out_kv_int("card", card); out_kv("driver", s); }
            free(s);
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "profile") == 0) {
        char *s = NULL;
        if (zenctl_gpu_amdgpu_get_power_profile(g, &s, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_int(&first, "card", card); out_json_field_string(&first, "profile", s); }
            else { out_kv_int("card", card); out_kv("profile", s); }
            free(s);
        } else rc = cli_err(json, &err);
    } else if (strcmp(what, "busy") == 0) {
        int v = 0;
        if (zenctl_gpu_amdgpu_get_busy_percent(g, &v, &err) == 0) {
            if (json) { out_json_ok_begin(); started_json = true; out_json_field_int(&first, "card", card); out_json_field_int(&first, "busy_percent", v); }
            else { out_kv_int("card", card); out_kv_int("busy_percent", v); }
        } else rc = cli_err(json, &err);
    } else {
        zenctl_gpu_close(g);
        return cli_usage(json,
            "zenctl gpu get <temp|fan|power|freq|driver|profile|busy|--all> [--card N]");
    }

    if (started_json) out_json_ok_end();
    zenctl_gpu_close(g);
    return rc;
}

/* ── gpu set ────────────────────────────────────────────────────── */

static int gpu_set(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 2)
        return cli_usage(json,
            "zenctl gpu set <fan|profile> <value> [--card N]");

    const char *what = argv[0];
    const char *val  = argv[1];
    int card = cli_opt_int(argc, argv, "--card", 0);

    if (cli_require_root(json)) return -1;

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_gpu_t *g = open_card(json, card, &err);
    if (!g) return -1;

    int rc = 0;
    if (strcmp(what, "fan") == 0) {
        if (strcmp(val, "auto") == 0) {
            if (dry_run) {
                cli_dryrun(json, "set fan auto");
            } else {
                if (zenctl_gpu_set_fan_auto(g, true, &err) == 0) {
                    if (json) { out_json_ok_begin(); fputs("\"fan=auto\"", stdout); out_json_ok_end(); }
                    else out_kv("set", "fan=auto");
                } else rc = cli_err(json, &err);
            }
        } else {
            long long pwm;
            if (cli_parse_int(val, &pwm) != 0 || pwm < 0 || pwm > 255) {
                cli_make_err(&err, ZENCTL_ERR_ERANGE,
                             "pwm must be 0-255 or 'auto'",
                             "gpu set fan");
                rc = cli_err(json, &err);
            } else if (dry_run) {
                char buf[64]; snprintf(buf, sizeof(buf), "set fan pwm=%lld", pwm);
                cli_dryrun(json, buf);
            } else {
                if (zenctl_gpu_set_fan_pwm(g, (int)pwm, &err) == 0) {
                    if (json) { out_json_ok_begin(); printf("\"fan_pwm\":%lld", pwm); out_json_ok_end(); }
                    else { out_kv_int("set fan_pwm", pwm); }
                } else rc = cli_err(json, &err);
            }
        }
    } else if (strcmp(what, "profile") == 0) {
        if (dry_run) {
            char buf[128]; snprintf(buf, sizeof(buf), "set profile=%s", val);
            cli_dryrun(json, buf);
        } else {
            if (zenctl_gpu_amdgpu_set_power_profile(g, val, &err) == 0) {
                if (json) { out_json_ok_begin(); out_json_escape("profile"); printf(":"); out_json_escape(val); out_json_ok_end(); }
                else { out_kv("set profile", val); }
            } else rc = cli_err(json, &err);
        }
    } else {
        rc = cli_usage(json, "zenctl gpu set <fan|profile> <value> [--card N]");
    }

    zenctl_gpu_close(g);
    return rc;
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_gpu(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)confirm;
    if (argc < 1)
        return cli_usage(json,
            "zenctl gpu <list|get|set> ...");
    if (strcmp(argv[0], "list") == 0) return gpu_list(json);
    if (strcmp(argv[0], "get")  == 0) return gpu_get(argc - 1, argv + 1, json);
    if (strcmp(argv[0], "set")  == 0) return gpu_set(argc - 1, argv + 1, json, dry_run);
    return cli_usage(json, "zenctl gpu <list|get|set> ...");
}
