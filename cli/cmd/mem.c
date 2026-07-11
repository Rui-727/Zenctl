/* cmd_mem.c - memory domain commands
 *
 * Subcommands:
 *   get  hugepages | thp | numa | swappiness | overcommit | --all
 *   set  hugepages | thp | swappiness | overcommit
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/mem.h"
#include "output.h"
#include "cmd_util.h"

/* ── hugepages ───────────────────────────────────────────────────── */

static int mem_get_hugepages(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    int64_t pages = 0, size = 0;
    if (zenctl_mem_get_nr_hugepages(&pages, &err) != 0) {
        cmd_print_err(json, &err, "cannot read nr_hugepages");
        return 1;
    }
    /* size is best-effort */
    zenctl_mem_get_hugepage_size(&size, NULL);

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "nr_hugepages", pages);
        if (size > 0) out_json_field_int(&f, "hugepage_size_bytes", size);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char pages_s[32], size_s[32];
        snprintf(pages_s, sizeof(pages_s), "%lld", (long long)pages);
        if (size > 0) {
            double mb = (double)size / (1024.0 * 1024.0);
            snprintf(size_s, sizeof(size_s), "%.0f MiB", mb);
        } else {
            snprintf(size_s, sizeof(size_s), "-");
        }
        out_kv("Hugepages", pages_s);
        out_kv("Hugepage size", size_s);
    }
    return 0;
}

static int mem_set_hugepages(int argc, char **argv, bool json, bool dry_run)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing page count");
        return 1;
    }
    const char *s = cmd_positional(argc, argv, 0);
    char *e = NULL;
    long long n = strtoll(s, &e, 10);
    if (e == s || *e != '\0' || n < 0) {
        cmd_print_err(json, NULL, "page count must be a non-negative integer");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_hugepages");
            out_json_field_int(&f, "nr_hugepages", (int64_t)n);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Would set nr_hugepages=%lld", n);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    if (zenctl_mem_set_nr_hugepages((int64_t)n, &err) != 0) {
        cmd_print_err(json, &err, "set nr_hugepages failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "nr_hugepages", (int64_t)n);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("nr_hugepages: %lld\n", n);
    }
    return 0;
}

/* ── THP ─────────────────────────────────────────────────────────── */

static const char *thp_mode_str(zenctl_thp_mode_t m)
{
    switch (m) {
    case ZENCTL_THP_ALWAYS:  return "always";
    case ZENCTL_THP_MADVISE: return "madvise";
    case ZENCTL_THP_NEVER:   return "never";
    default:                 return "unknown";
    }
}

static int mem_get_thp(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    zenctl_thp_mode_t m;
    if (zenctl_mem_get_thp(&m, &err) != 0) {
        cmd_print_err(json, &err, "cannot read THP mode");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "thp", thp_mode_str(m));
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("THP", thp_mode_str(m));
    }
    return 0;
}

static int mem_set_thp(int argc, char **argv, bool json, bool dry_run)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing THP mode");
        return 1;
    }
    const char *s = cmd_positional(argc, argv, 0);
    zenctl_thp_mode_t m;
    if      (strcmp(s, "always")  == 0) m = ZENCTL_THP_ALWAYS;
    else if (strcmp(s, "madvise") == 0) m = ZENCTL_THP_MADVISE;
    else if (strcmp(s, "never")   == 0) m = ZENCTL_THP_NEVER;
    else {
        cmd_print_err(json, NULL, "THP mode must be always|madvise|never");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_thp");
            out_json_field_string(&f, "thp", s);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Would set THP=%s", s);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    if (zenctl_mem_set_thp(m, &err) != 0) {
        cmd_print_err(json, &err, "set THP failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "thp", s);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("THP: %s\n", s);
    }
    return 0;
}

/* ── NUMA ────────────────────────────────────────────────────────── */

static int mem_get_numa(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    int n = 0;
    if (zenctl_mem_numa_node_count(&n, &err) != 0) {
        cmd_print_err(json, &err, "cannot read NUMA node count");
        return 1;
    }
    if (n == 0) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_int(&f, "numa_nodes", 0);
            out_json_field_array_begin(&f, "nodes");
            out_json_close_array();
            out_json_close_object();
            out_json_ok_end();
        } else {
            out_kv("NUMA nodes", "0 (non-NUMA system)");
        }
        return 0;
    }

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "numa_nodes", n);
        out_json_field_array_begin(&f, "nodes");
        bool arr_first = true;
        for (int i = 0; i < n; i++) {
            zenctl_numa_node_t nd;
            if (zenctl_mem_numa_get_node(i, &nd, &err) != 0) continue;
            out_json_separator(&arr_first);
            out_json_open_object();
            bool of = true;
            out_json_field_int(&of, "node", nd.node_id);
            out_json_field_int(&of, "mem_total_bytes", nd.mem_total);
            out_json_field_int(&of, "mem_free_bytes", nd.mem_free);
            out_json_field_string(&of, "cpumask", nd.cpumask);
            out_json_close_object();
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "NODE", "MEM_TOTAL", "MEM_FREE", "CPUMASK" };
        out_table_header(h, 4);
        for (int i = 0; i < n; i++) {
            zenctl_numa_node_t nd;
            if (zenctl_mem_numa_get_node(i, &nd, &err) != 0) continue;
            char ibuf[16], tbuf[32], fbuf[32];
            snprintf(ibuf, sizeof(ibuf), "%d", nd.node_id);
            double tmb = (double)nd.mem_total / (1024.0 * 1024.0);
            double fmb = (double)nd.mem_free  / (1024.0 * 1024.0);
            snprintf(tbuf, sizeof(tbuf), "%.0f MiB", tmb);
            snprintf(fbuf, sizeof(fbuf), "%.0f MiB", fmb);
            const char *row[] = { ibuf, tbuf, fbuf, nd.cpumask };
            out_table_row(row, 4);
        }
    }
    return 0;
}

/* ── swappiness ──────────────────────────────────────────────────── */

static int mem_get_swappiness(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    int v = 0;
    if (zenctl_mem_get_swappiness(&v, &err) != 0) {
        cmd_print_err(json, &err, "cannot read swappiness");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "swappiness", v);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[16];
        snprintf(b, sizeof(b), "%d", v);
        out_kv("Swappiness", b);
    }
    return 0;
}

static int mem_set_swappiness(int argc, char **argv, bool json, bool dry_run)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing value");
        return 1;
    }
    const char *s = cmd_positional(argc, argv, 0);
    char *e = NULL;
    long v = strtol(s, &e, 10);
    if (e == s || *e != '\0' || v < 0 || v > 200) {
        cmd_print_err(json, NULL, "swappiness must be 0..200");
        return 1;
    }
    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_swappiness");
            out_json_field_int(&f, "swappiness", (int64_t)v);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Would set swappiness=%ld", v);
            out_dryrun(msg);
        }
        return 0;
    }
    zenctl_err_t err;
    if (zenctl_mem_set_swappiness((int)v, &err) != 0) {
        cmd_print_err(json, &err, "set swappiness failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "swappiness", (int64_t)v);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("swappiness: %ld\n", v);
    }
    return 0;
}

/* ── overcommit ──────────────────────────────────────────────────── */

static const char *overcommit_str(int v)
{
    switch (v) {
    case 0: return "heuristic";
    case 1: return "always";
    case 2: return "never";
    default: return "unknown";
    }
}

static int mem_get_overcommit(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    int v = 0;
    if (zenctl_mem_get_overcommit(&v, &err) != 0) {
        cmd_print_err(json, &err, "cannot read overcommit");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "overcommit", v);
        out_json_field_string(&f, "mode", overcommit_str(v));
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32];
        snprintf(b, sizeof(b), "%d (%s)", v, overcommit_str(v));
        out_kv("Overcommit", b);
    }
    return 0;
}

static int mem_set_overcommit(int argc, char **argv, bool json, bool dry_run)
{
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing value (0|1|2)");
        return 1;
    }
    const char *s = cmd_positional(argc, argv, 0);
    char *e = NULL;
    long v = strtol(s, &e, 10);
    if (e == s || *e != '\0' || v < 0 || v > 2) {
        cmd_print_err(json, NULL, "overcommit must be 0, 1, or 2");
        return 1;
    }
    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_overcommit");
            out_json_field_int(&f, "overcommit", (int64_t)v);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Would set overcommit=%ld (%s)", v, overcommit_str((int)v));
            out_dryrun(msg);
        }
        return 0;
    }
    zenctl_err_t err;
    if (zenctl_mem_set_overcommit((int)v, &err) != 0) {
        cmd_print_err(json, &err, "set overcommit failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "overcommit", (int64_t)v);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("overcommit: %ld (%s)\n", v, overcommit_str((int)v));
    }
    return 0;
}

/* ── get --all ───────────────────────────────────────────────────── */

static int mem_get_all(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    int64_t pages = 0, hsize = 0;
    zenctl_thp_mode_t thp = ZENCTL_THP_NEVER;
    int swap = 0, oc = 0;
    int nnode = 0;
    bool have_pages = (zenctl_mem_get_nr_hugepages(&pages, &err) == 0);
    bool have_hsize = (zenctl_mem_get_hugepage_size(&hsize, &err) == 0);
    bool have_thp   = (zenctl_mem_get_thp(&thp, &err) == 0);
    bool have_swap  = (zenctl_mem_get_swappiness(&swap, &err) == 0);
    bool have_oc    = (zenctl_mem_get_overcommit(&oc, &err) == 0);
    bool have_numa  = (zenctl_mem_numa_node_count(&nnode, &err) == 0);

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        if (have_pages) out_json_field_int(&f, "nr_hugepages", pages);
        if (have_hsize) out_json_field_int(&f, "hugepage_size_bytes", hsize);
        if (have_thp)   out_json_field_string(&f, "thp", thp_mode_str(thp));
        if (have_swap)  out_json_field_int(&f, "swappiness", swap);
        if (have_oc) {
            out_json_field_int(&f, "overcommit", oc);
            out_json_field_string(&f, "overcommit_mode", overcommit_str(oc));
        }
        if (have_numa) {
            out_json_field_int(&f, "numa_nodes", nnode);
            out_json_field_array_begin(&f, "numa");
            bool arr_first = true;
            for (int i = 0; i < nnode; i++) {
                zenctl_numa_node_t nd;
                if (zenctl_mem_numa_get_node(i, &nd, &err) != 0) continue;
                out_json_separator(&arr_first);
                out_json_open_object();
                bool of = true;
                out_json_field_int(&of, "node", nd.node_id);
                out_json_field_int(&of, "mem_total_bytes", nd.mem_total);
                out_json_field_int(&of, "mem_free_bytes", nd.mem_free);
                out_json_field_string(&of, "cpumask", nd.cpumask);
                out_json_close_object();
            }
            out_json_close_array();
        }
        out_json_close_object();
        out_json_ok_end();
    } else {
        if (have_pages) {
            char b[32]; snprintf(b, sizeof(b), "%lld", (long long)pages);
            out_kv("Hugepages", b);
        }
        if (have_hsize) {
            char b[32]; double mb = (double)hsize / (1024.0 * 1024.0);
            snprintf(b, sizeof(b), "%.0f MiB", mb);
            out_kv("Hugepage size", b);
        }
        if (have_thp)   out_kv("THP", thp_mode_str(thp));
        if (have_swap)  { char b[16]; snprintf(b, sizeof(b), "%d", swap);  out_kv("Swappiness", b); }
        if (have_oc)    {
            char b[32]; snprintf(b, sizeof(b), "%d (%s)", oc, overcommit_str(oc));
            out_kv("Overcommit", b);
        }
        if (have_numa)  {
            char b[16]; snprintf(b, sizeof(b), "%d", nnode);
            out_kv("NUMA nodes", b);
        }
    }
    return 0;
}

/* ── top-level dispatch ──────────────────────────────────────────── */

int cmd_mem(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)confirm;
    if (argc < 1) {
        cmd_print_err(json, NULL, "missing subcommand (try 'zenctl mem get ...')");
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
            return mem_get_all(rargc, rargv, json);
        if (strcmp(what, "hugepages")  == 0) return mem_get_hugepages(rargc, rargv, json);
        if (strcmp(what, "thp")        == 0) return mem_get_thp(rargc, rargv, json);
        if (strcmp(what, "numa")       == 0) return mem_get_numa(rargc, rargv, json);
        if (strcmp(what, "swappiness") == 0) return mem_get_swappiness(rargc, rargv, json);
        if (strcmp(what, "overcommit") == 0) return mem_get_overcommit(rargc, rargv, json);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown mem get target '%s'. Valid: hugepages, thp, numa, swappiness, overcommit", what);
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
        if (strcmp(what, "hugepages")  == 0) return mem_set_hugepages(rargc, rargv, json, dry_run);
        if (strcmp(what, "thp")        == 0) return mem_set_thp(rargc, rargv, json, dry_run);
        if (strcmp(what, "swappiness") == 0) return mem_set_swappiness(rargc, rargv, json, dry_run);
        if (strcmp(what, "overcommit") == 0) return mem_set_overcommit(rargc, rargv, json, dry_run);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown mem set target '%s'. Valid: hugepages, thp, swappiness, overcommit", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "unknown mem subcommand '%s'. Try: get, set", sub);
    cmd_print_err(json, NULL, buf);
    return 1;
}
