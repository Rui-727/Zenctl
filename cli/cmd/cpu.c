/* cmd_cpu.c - CPU domain commands
 *
 * Subcommands:
 *   get  governor | freq | online | smt | cstate | --all
 *   set  governor | freq-min | freq-max | online | smt | cstate
 *
 * Frequency parsing accepts plain Hz, "3600K", "3600M", "3.6GHz",
 * "3600MHz", "3.6G", etc. Internally everything is Hz.
 *
 * Per-CPU operations accept --cpu N. The special value "all" (and the
 * default when --cpu is omitted) iterates every online CPU.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "zenctl/zenctl.h"
#include "zenctl/cpu.h"
#include "output.h"
#include "cmd_util.h"

/* ── Frequency parsing ───────────────────────────────────────────── */

/* Parse a frequency string to Hz. Accepts:
 *   "3600000"     -> Hz (plain)
 *   "3600MHz"     -> 3.6e9 Hz
 *   "3.6GHz"      -> 3.6e9 Hz
 *   "3600M"       -> 3.6e9 Hz
 *   "3.6G"        -> 3.6e9 Hz
 *   "3600K"/kHz   -> 3.6e6 Hz
 *   Trailing "Hz" after a unit (e.g. "3.6GHz") is tolerated.
 * Returns 0 on success, -1 on error.
 */
static int parse_freq(const char *s, int64_t *out_hz)
{
    if (!s || !*s) return -1;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return -1;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end == '\0') {
        /* No suffix - assume Hz. */
        *out_hz = (int64_t)v;
        return 0;
    }
    char u = (char)tolower((unsigned char)*end);
    if (u == 'm') {
        *out_hz = (int64_t)(v * 1e6);
        return 0;
    }
    if (u == 'g') {
        *out_hz = (int64_t)(v * 1e9);
        return 0;
    }
    if (u == 'k') {
        *out_hz = (int64_t)(v * 1e3);
        return 0;
    }
    return -1;
}

/* ── CPU index parsing ───────────────────────────────────────────── */

/* Parse the --cpu option. Returns:
 *   >=0: specific CPU index
 *   -1 : "all" or not specified (iterate all CPUs)
 *   -2 : parse error
 */
static int parse_cpu_arg(int argc, char **argv)
{
    const char *v = NULL;
    int rc = cmd_opt_str(argc, argv, "--cpu", &v);
    if (rc == 0) return -1;
    if (rc < 0)  return -2;
    if (strcmp(v, "all") == 0) return -1;
    char *e = NULL;
    long n = strtol(v, &e, 10);
    if (e == v || *e != '\0' || n < 0) return -2;
    return (int)n;
}

/* ── C-state enumeration ─────────────────────────────────────────── */

/* Count C-states for a CPU by trying state0, state1, ... until one is
 * missing. */
static int count_cstates(zenctl_cpu_t *cpu)
{
    int n = 0;
    for (;;) {
        zenctl_err_t err;
        bool dummy;
        if (zenctl_cpu_get_cstate_disable(cpu, n, &dummy, &err) != 0)
            break;
        n++;
        if (n > 64) break;  /* sanity */
    }
    return n;
}

/* ── Per-CPU dump used by --all and friends ──────────────────────── */

/* Append a single CPU's snapshot to a JSON array. The array's "first"
 * flag tracks comma separation between elements. */
static void emit_cpu_json(zenctl_cpu_t *cpu, int idx, bool *arr_first)
{
    zenctl_err_t err;
    out_json_separator(arr_first);
    out_json_open_object();
    bool f = true;
    out_json_field_int(&f, "cpu", idx);

    char *gov = NULL;
    if (zenctl_cpu_get_governor(cpu, &gov, &err) == 0) {
        out_json_field_string(&f, "governor", gov);
        free(gov);
    }

    int64_t fmin = 0, fmax = 0;
    if (zenctl_cpu_get_freq_min(cpu, &fmin, &err) == 0)
        out_json_field_int(&f, "freq_min_hz", fmin);
    if (zenctl_cpu_get_freq_max(cpu, &fmax, &err) == 0)
        out_json_field_int(&f, "freq_max_hz", fmax);

    bool online = true;
    if (zenctl_cpu_get_online(cpu, &online, &err) == 0)
        out_json_field_bool(&f, "online", online);

    int ncstate = count_cstates(cpu);
    if (ncstate > 0) {
        out_json_field_array_begin(&f, "cstates");
        bool cf = true;
        for (int s = 0; s < ncstate; s++) {
            bool dis = false;
            if (zenctl_cpu_get_cstate_disable(cpu, s, &dis, &err) != 0)
                continue;
            out_json_separator(&cf);
            out_json_open_object();
            bool sf = true;
            out_json_field_int(&sf, "state", s);
            out_json_field_bool(&sf, "disabled", dis);
            out_json_close_object();
        }
        out_json_close_array();
    }
    out_json_close_object();
}

/* Print one CPU row in a table for `cpu get --all`. */
static void print_cpu_table_row(zenctl_cpu_t *cpu, int idx)
{
    zenctl_err_t err;
    char gov_buf[64]   = "-";
    char fmin_buf[32]  = "-";
    char fmax_buf[32]  = "-";
    char on_buf[8]     = "-";
    char idx_buf[16];
    snprintf(idx_buf, sizeof(idx_buf), "%d", idx);

    char *gov = NULL;
    if (zenctl_cpu_get_governor(cpu, &gov, &err) == 0) {
        snprintf(gov_buf, sizeof(gov_buf), "%s", gov);
        free(gov);
    }
    int64_t fmin = 0, fmax = 0;
    if (zenctl_cpu_get_freq_min(cpu, &fmin, &err) == 0)
        cmd_format_hz(fmin, fmin_buf, sizeof(fmin_buf));
    if (zenctl_cpu_get_freq_max(cpu, &fmax, &err) == 0)
        cmd_format_hz(fmax, fmax_buf, sizeof(fmax_buf));
    bool online = true;
    if (zenctl_cpu_get_online(cpu, &online, &err) == 0)
        snprintf(on_buf, sizeof(on_buf), "%s", online ? "on" : "off");

    const char *row[] = { idx_buf, gov_buf, fmin_buf, fmax_buf, on_buf };
    out_table_row(row, 5);
}

/* ── get governor ────────────────────────────────────────────────── */

static int cpu_get_governor(int argc, char **argv, bool json)
{
    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    int ncpu = cmd_count_cpus();
    int start = (cpu_idx == -1) ? 0 : cpu_idx;
    int end   = (cpu_idx == -1) ? ncpu : cpu_idx + 1;

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_array_begin(&f, "cpus");
        bool arr_first = true;
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            char *gov = NULL;
            if (zenctl_cpu_get_governor(cpu, &gov, &err) == 0) {
                out_json_separator(&arr_first);
                out_json_open_object();
                bool of = true;
                out_json_field_int(&of, "cpu", c);
                out_json_field_string(&of, "governor", gov);
                out_json_close_object();
                free(gov);
            }
            zenctl_cpu_close(cpu);
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "CPU", "GOVERNOR" };
        out_table_header(h, 2);
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            char *gov = NULL;
            if (zenctl_cpu_get_governor(cpu, &gov, &err) == 0) {
                char idx_buf[16];
                snprintf(idx_buf, sizeof(idx_buf), "%d", c);
                const char *row[] = { idx_buf, gov };
                out_table_row(row, 2);
                free(gov);
            }
            zenctl_cpu_close(cpu);
        }
    }
    return 0;
}

/* ── set governor ────────────────────────────────────────────────── */

static int cpu_set_governor(int argc, char **argv, bool json, bool dry_run)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing governor name");
        return 1;
    }
    const char *gov = cmd_positional(argc, argv, 0);
    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    int ncpu = cmd_count_cpus();
    int start = (cpu_idx == -1) ? 0 : cpu_idx;
    int end   = (cpu_idx == -1) ? ncpu : cpu_idx + 1;

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_governor");
            out_json_field_string(&f, "governor", gov);
            out_json_field_int(&f, "cpu_start", start);
            out_json_field_int(&f, "cpu_end", end);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set governor='%s' on CPUs %d..%d",
                     gov, start, end - 1);
            out_dryrun(msg);
        }
        return 0;
    }

    int failures = 0;
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_array_begin(&f, "results");
        bool arr_first = true;
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) { failures++; continue; }
            int rc = zenctl_cpu_set_governor(cpu, gov, &err);
            out_json_separator(&arr_first);
            out_json_open_object();
            bool of = true;
            out_json_field_int(&of, "cpu", c);
            if (rc == 0) {
                out_json_field_bool(&of, "ok", true);
            } else {
                out_json_field_bool(&of, "ok", false);
                out_json_field_int(&of, "code", err.code);
                out_json_field_string(&of, "error", err.message);
                failures++;
            }
            out_json_close_object();
            zenctl_cpu_close(cpu);
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) {
                char buf[64];
                snprintf(buf, sizeof(buf), "cpu%d: open failed", c);
                out_err(buf);
                failures++;
                continue;
            }
            if (zenctl_cpu_set_governor(cpu, gov, &err) != 0) {
                char buf[512];
                snprintf(buf, sizeof(buf), "cpu%d: %s", c, err.message);
                out_err(buf);
                failures++;
            } else {
                printf("cpu%d: governor=%s\n", c, gov);
            }
            zenctl_cpu_close(cpu);
        }
    }
    return failures == 0 ? 0 : 1;
}

/* ── get freq ────────────────────────────────────────────────────── */

static int cpu_get_freq(int argc, char **argv, bool json)
{
    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    int ncpu = cmd_count_cpus();
    int start = (cpu_idx == -1) ? 0 : cpu_idx;
    int end   = (cpu_idx == -1) ? ncpu : cpu_idx + 1;

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_array_begin(&f, "cpus");
        bool arr_first = true;
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            int64_t fmin = 0, fmax = 0;
            bool ok_min = (zenctl_cpu_get_freq_min(cpu, &fmin, &err) == 0);
            bool ok_max = (zenctl_cpu_get_freq_max(cpu, &fmax, &err) == 0);
            if (ok_min || ok_max) {
                out_json_separator(&arr_first);
                out_json_open_object();
                bool of = true;
                out_json_field_int(&of, "cpu", c);
                if (ok_min) out_json_field_int(&of, "freq_min_hz", fmin);
                if (ok_max) out_json_field_int(&of, "freq_max_hz", fmax);
                out_json_close_object();
            }
            zenctl_cpu_close(cpu);
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "CPU", "FREQ_MIN", "FREQ_MAX" };
        out_table_header(h, 3);
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            int64_t fmin = 0, fmax = 0;
            char fmin_s[32] = "-", fmax_s[32] = "-";
            char idx_s[16];
            snprintf(idx_s, sizeof(idx_s), "%d", c);
            if (zenctl_cpu_get_freq_min(cpu, &fmin, &err) == 0)
                cmd_format_hz(fmin, fmin_s, sizeof(fmin_s));
            if (zenctl_cpu_get_freq_max(cpu, &fmax, &err) == 0)
                cmd_format_hz(fmax, fmax_s, sizeof(fmax_s));
            const char *row[] = { idx_s, fmin_s, fmax_s };
            out_table_row(row, 3);
            zenctl_cpu_close(cpu);
        }
    }
    return 0;
}

/* ── set freq-min / freq-max ─────────────────────────────────────── */

static int cpu_set_freq(int argc, char **argv, bool json, bool dry_run,
                        bool is_max)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing frequency value");
        return 1;
    }
    const char *freq_s = cmd_positional(argc, argv, 0);
    int64_t hz = 0;
    if (parse_freq(freq_s, &hz) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "invalid frequency '%s' (try 3.6GHz, 3600M, 3600000)", freq_s);
        cmd_print_err(json, NULL, buf);
        return 1;
    }
    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    int ncpu = cmd_count_cpus();
    int start = (cpu_idx == -1) ? 0 : cpu_idx;
    int end   = (cpu_idx == -1) ? ncpu : cpu_idx + 1;

    const char *which = is_max ? "freq-max" : "freq-min";

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_freq");
            out_json_field_string(&f, "which", which);
            out_json_field_int(&f, "hz", hz);
            out_json_field_int(&f, "cpu_start", start);
            out_json_field_int(&f, "cpu_end", end);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char hz_buf[32];
            cmd_format_hz(hz, hz_buf, sizeof(hz_buf));
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set %s=%s (%lld Hz) on CPUs %d..%d",
                     which, hz_buf, (long long)hz, start, end - 1);
            out_dryrun(msg);
        }
        return 0;
    }

    int failures = 0;
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_array_begin(&f, "results");
        bool arr_first = true;
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) { failures++; continue; }
            int rc = is_max
                ? zenctl_cpu_set_freq_max(cpu, hz, &err)
                : zenctl_cpu_set_freq_min(cpu, hz, &err);
            out_json_separator(&arr_first);
            out_json_open_object();
            bool of = true;
            out_json_field_int(&of, "cpu", c);
            if (rc == 0) {
                out_json_field_bool(&of, "ok", true);
            } else {
                out_json_field_bool(&of, "ok", false);
                out_json_field_int(&of, "code", err.code);
                out_json_field_string(&of, "error", err.message);
                failures++;
            }
            out_json_close_object();
            zenctl_cpu_close(cpu);
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) {
                char buf[64];
                snprintf(buf, sizeof(buf), "cpu%d: open failed", c);
                out_err(buf);
                failures++;
                continue;
            }
            int rc = is_max
                ? zenctl_cpu_set_freq_max(cpu, hz, &err)
                : zenctl_cpu_set_freq_min(cpu, hz, &err);
            if (rc != 0) {
                char buf[512];
                snprintf(buf, sizeof(buf), "cpu%d: %s", c, err.message);
                out_err(buf);
                failures++;
            } else {
                char hz_buf[32];
                cmd_format_hz(hz, hz_buf, sizeof(hz_buf));
                printf("cpu%d: %s=%s\n", c, which, hz_buf);
            }
            zenctl_cpu_close(cpu);
        }
    }
    return failures == 0 ? 0 : 1;
}

/* ── get online ──────────────────────────────────────────────────── */

static int cpu_get_online(int argc, char **argv, bool json)
{
    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    int ncpu = cmd_count_cpus();
    int start = (cpu_idx == -1) ? 0 : cpu_idx;
    int end   = (cpu_idx == -1) ? ncpu : cpu_idx + 1;

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_array_begin(&f, "cpus");
        bool arr_first = true;
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            bool on = true;
            if (zenctl_cpu_get_online(cpu, &on, &err) == 0) {
                out_json_separator(&arr_first);
                out_json_open_object();
                bool of = true;
                out_json_field_int(&of, "cpu", c);
                out_json_field_bool(&of, "online", on);
                out_json_close_object();
            }
            zenctl_cpu_close(cpu);
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "CPU", "ONLINE" };
        out_table_header(h, 2);
        for (int c = start; c < end; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            bool on = true;
            if (zenctl_cpu_get_online(cpu, &on, &err) == 0) {
                char idx_s[16];
                snprintf(idx_s, sizeof(idx_s), "%d", c);
                const char *row[] = { idx_s, on ? "on" : "off" };
                out_table_row(row, 2);
            }
            zenctl_cpu_close(cpu);
        }
    }
    return 0;
}

/* ── set online (destructive) ────────────────────────────────────── */

static bool ask_confirm(const char *what, bool confirm_flag)
{
    if (confirm_flag) return true;
    fprintf(stderr, "About to %s. Continue? [y/N] ", what);
    fflush(stderr);
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    return (buf[0] == 'y' || buf[0] == 'Y');
}

static int cpu_set_online(int argc, char **argv, bool json,
                          bool dry_run, bool confirm)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing on/off argument");
        return 1;
    }
    const char *val = cmd_positional(argc, argv, 0);
    bool on;
    if (strcmp(val, "on") == 0)       on = true;
    else if (strcmp(val, "off") == 0) on = false;
    else {
        cmd_print_err(json, NULL, "argument must be 'on' or 'off'");
        return 1;
    }

    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    if (cpu_idx == -1) {
        /* --cpu all is too dangerous for online ops; require explicit. */
        cmd_print_err(json, NULL,
                      "online set requires explicit --cpu N (use --cpu all is not supported for safety)");
        return 1;
    }
    int c = cpu_idx;

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_online");
            out_json_field_int(&f, "cpu", c);
            out_json_field_bool(&f, "on", on);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Would set cpu%d %s", c, on ? "online" : "offline");
            out_dryrun(msg);
        }
        return 0;
    }

    char what[128];
    snprintf(what, sizeof(what), "%sline cpu%d", on ? "on" : "off", c);
    if (!on && !ask_confirm(what, confirm)) {
        if (json) out_json_error(3, "operation cancelled by user");
        else      out_err("operation cancelled");
        return 1;
    }

    zenctl_err_t err;
    zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
    if (!cpu) {
        cmd_print_err(json, &err, "cpu open failed");
        return 1;
    }
    int rc = zenctl_cpu_set_online(cpu, on, &err);
    zenctl_cpu_close(cpu);
    if (rc != 0) {
        cmd_print_err(json, &err, "set online failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "cpu", c);
        out_json_field_bool(&f, "online", on);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("cpu%d: %s\n", c, on ? "online" : "offline");
    }
    return 0;
}

/* ── get smt ─────────────────────────────────────────────────────── */

static int cpu_get_smt(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    /* SMT is global; use any CPU handle. */
    zenctl_cpu_t *cpu = zenctl_cpu_open(0, &err);
    if (!cpu) {
        cmd_print_err(json, &err, "cannot open cpu0");
        return 1;
    }
    bool active = false;
    int rc = zenctl_cpu_get_smt_active(cpu, &active, &err);
    zenctl_cpu_close(cpu);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read SMT state");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_bool(&f, "smt_active", active);
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("SMT active", active ? "yes" : "no");
    }
    return 0;
}

/* ── set smt (destructive) ───────────────────────────────────────── */

static int cpu_set_smt(int argc, char **argv, bool json,
                       bool dry_run, bool confirm)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing on/off argument");
        return 1;
    }
    const char *val = cmd_positional(argc, argv, 0);
    bool on;
    if (strcmp(val, "on") == 0)       on = true;
    else if (strcmp(val, "off") == 0) on = false;
    else {
        cmd_print_err(json, NULL, "argument must be 'on' or 'off'");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_smt");
            out_json_field_bool(&f, "on", on);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Would set SMT %s", on ? "on" : "off");
            out_dryrun(msg);
        }
        return 0;
    }

    if (!on && !ask_confirm("disable SMT (this offlines sibling threads)",
                            confirm)) {
        if (json) out_json_error(3, "operation cancelled by user");
        else      out_err("operation cancelled");
        return 1;
    }

    zenctl_err_t err;
    zenctl_cpu_t *cpu = zenctl_cpu_open(0, &err);
    if (!cpu) {
        cmd_print_err(json, &err, "cannot open cpu0");
        return 1;
    }
    int rc = zenctl_cpu_set_smt_active(cpu, on, &err);
    zenctl_cpu_close(cpu);
    if (rc != 0) {
        cmd_print_err(json, &err, "set SMT failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_bool(&f, "smt_active", on);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("SMT: %s\n", on ? "on" : "off");
    }
    return 0;
}

/* ── get cstate ──────────────────────────────────────────────────── */

static int cpu_get_cstate(int argc, char **argv, bool json)
{
    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    /* Default to cpu 0 if not specified. */
    if (cpu_idx == -1) cpu_idx = 0;

    long state_filter = -1;
    if (cmd_opt_int(argc, argv, "--state", &state_filter) < 0) {
        cmd_print_err(json, NULL, "invalid --state value");
        return 1;
    }

    zenctl_err_t err;
    zenctl_cpu_t *cpu = zenctl_cpu_open(cpu_idx, &err);
    if (!cpu) {
        cmd_print_err(json, &err, "cpu open failed");
        return 1;
    }

    int n = count_cstates(cpu);
    int s_start = (state_filter >= 0) ? (int)state_filter : 0;
    int s_end   = (state_filter >= 0) ? (int)state_filter + 1 : n;

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "cpu", cpu_idx);
        out_json_field_array_begin(&f, "cstates");
        bool arr_first = true;
        for (int s = s_start; s < s_end; s++) {
            bool dis = false;
            if (zenctl_cpu_get_cstate_disable(cpu, s, &dis, &err) != 0)
                continue;
            out_json_separator(&arr_first);
            out_json_open_object();
            bool of = true;
            out_json_field_int(&of, "state", s);
            out_json_field_bool(&of, "disabled", dis);
            out_json_close_object();
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "CPU", "STATE", "DISABLED" };
        out_table_header(h, 3);
        for (int s = s_start; s < s_end; s++) {
            bool dis = false;
            if (zenctl_cpu_get_cstate_disable(cpu, s, &dis, &err) != 0)
                continue;
            char cbuf[16], sbuf[16], dbuf[8];
            snprintf(cbuf, sizeof(cbuf), "%d", cpu_idx);
            snprintf(sbuf, sizeof(sbuf), "%d", s);
            snprintf(dbuf, sizeof(dbuf), "%s", dis ? "yes" : "no");
            const char *row[] = { cbuf, sbuf, dbuf };
            out_table_row(row, 3);
        }
    }
    zenctl_cpu_close(cpu);
    return 0;
}

/* ── set cstate ──────────────────────────────────────────────────── */

static int cpu_set_cstate(int argc, char **argv, bool json, bool dry_run)
{
    if (cmd_positional_count(argc, argv) < 2) {
        cmd_print_err(json, NULL, "usage: set cstate <state> <enable|disable> [--cpu N]");
        return 1;
    }
    const char *state_s = cmd_positional(argc, argv, 0);
    const char *val     = cmd_positional(argc, argv, 1);
    char *e = NULL;
    long state = strtol(state_s, &e, 10);
    if (e == state_s || *e != '\0' || state < 0) {
        cmd_print_err(json, NULL, "invalid state index");
        return 1;
    }
    bool disable;
    if (strcmp(val, "disable") == 0)       disable = true;
    else if (strcmp(val, "enable") == 0)   disable = false;
    else {
        cmd_print_err(json, NULL, "argument must be 'enable' or 'disable'");
        return 1;
    }

    int cpu_idx = parse_cpu_arg(argc, argv);
    if (cpu_idx == -2) {
        cmd_print_err(json, NULL, "invalid --cpu value");
        return 1;
    }
    if (cpu_idx == -1) cpu_idx = 0;

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_cstate");
            out_json_field_int(&f, "cpu", cpu_idx);
            out_json_field_int(&f, "state", state);
            out_json_field_bool(&f, "disable", disable);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Would %s C-state %ld on cpu%d",
                     disable ? "disable" : "enable", state, cpu_idx);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_cpu_t *cpu = zenctl_cpu_open(cpu_idx, &err);
    if (!cpu) {
        cmd_print_err(json, &err, "cpu open failed");
        return 1;
    }
    int rc = zenctl_cpu_set_cstate_disable(cpu, (int)state, disable, &err);
    zenctl_cpu_close(cpu);
    if (rc != 0) {
        cmd_print_err(json, &err, "set cstate failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "cpu", cpu_idx);
        out_json_field_int(&f, "state", (int)state);
        out_json_field_bool(&f, "disabled", disable);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("cpu%d: C-state %ld %s\n", cpu_idx, state,
               disable ? "disabled" : "enabled");
    }
    return 0;
}

/* ── get --all ───────────────────────────────────────────────────── */

static int cpu_get_all(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    int ncpu = cmd_count_cpus();

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        /* Global SMT state at top level. */
        zenctl_err_t err;
        zenctl_cpu_t *cpu0 = zenctl_cpu_open(0, &err);
        if (cpu0) {
            bool smt = false;
            if (zenctl_cpu_get_smt_active(cpu0, &smt, &err) == 0)
                out_json_field_bool(&f, "smt_active", smt);
            zenctl_cpu_close(cpu0);
        }
        out_json_field_array_begin(&f, "cpus");
        bool arr_first = true;
        for (int c = 0; c < ncpu; c++) {
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            emit_cpu_json(cpu, c, &arr_first);
            zenctl_cpu_close(cpu);
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "CPU", "GOVERNOR", "FREQ_MIN", "FREQ_MAX", "ONLINE" };
        out_table_header(h, 5);
        for (int c = 0; c < ncpu; c++) {
            zenctl_err_t err;
            zenctl_cpu_t *cpu = zenctl_cpu_open(c, &err);
            if (!cpu) continue;
            print_cpu_table_row(cpu, c);
            zenctl_cpu_close(cpu);
        }
    }
    return 0;
}

/* ── top-level dispatch ──────────────────────────────────────────── */

int cmd_cpu(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1) {
        cmd_print_err(json, NULL, "missing subcommand (try 'zenctl cpu get ...')");
        return 1;
    }
    const char *sub = argv[0];

    if (strcmp(sub, "get") == 0) {
        if (argc < 2) {
            cmd_print_err(json, NULL, "missing get target");
            return 1;
        }
        const char *what = argv[1];
        int rargc = argc - 2;
        char **rargv = argv + 2;
        if (strcmp(what, "--all") == 0 || strcmp(what, "-a") == 0)
            return cpu_get_all(rargc, rargv, json);
        if (strcmp(what, "governor") == 0) return cpu_get_governor(rargc, rargv, json);
        if (strcmp(what, "freq")     == 0) return cpu_get_freq(rargc, rargv, json);
        if (strcmp(what, "online")   == 0) return cpu_get_online(rargc, rargv, json);
        if (strcmp(what, "smt")      == 0) return cpu_get_smt(rargc, rargv, json);
        if (strcmp(what, "cstate")   == 0) return cpu_get_cstate(rargc, rargv, json);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown cpu get target '%s'. Valid: governor, freq, online, smt, cstate", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }

    if (strcmp(sub, "set") == 0) {
        if (argc < 2) {
            cmd_print_err(json, NULL, "missing set target");
            return 1;
        }
        const char *what = argv[1];
        int rargc = argc - 2;
        char **rargv = argv + 2;
        if (strcmp(what, "governor") == 0) return cpu_set_governor(rargc, rargv, json, dry_run);
        if (strcmp(what, "freq-min") == 0) return cpu_set_freq(rargc, rargv, json, dry_run, false);
        if (strcmp(what, "freq-max") == 0) return cpu_set_freq(rargc, rargv, json, dry_run, true);
        if (strcmp(what, "online")   == 0) return cpu_set_online(rargc, rargv, json, dry_run, confirm);
        if (strcmp(what, "smt")      == 0) return cpu_set_smt(rargc, rargv, json, dry_run, confirm);
        if (strcmp(what, "cstate")   == 0) return cpu_set_cstate(rargc, rargv, json, dry_run);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown cpu set target '%s'. Valid: governor, freq-min, freq-max, online, smt, cstate", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "unknown cpu subcommand '%s' (try 'get' or 'set')", sub);
    cmd_print_err(json, NULL, buf);
    return 1;
}
