/* cmd_net.c - network domain commands
 *
 * Subcommands:
 *   get  ring | offload | link | mtu | irq
 *   set  ring | offload | mtu | irq-affinity
 *   list
 *
 * The library does not yet expose interface enumeration; `list` scans
 * /sys/class/net/ directly (read-only directory listing).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/net.h"
#include "output.h"
#include "cmd_util.h"

/* ── helpers ─────────────────────────────────────────────────────── */

static int get_iface_arg(int argc, char **argv, const char **out)
{
    return cmd_opt_str(argc, argv, "--iface", out);
}

static int parse_on_off(const char *s, bool *out)
{
    if (strcmp(s, "on") == 0)  { *out = true;  return 0; }
    if (strcmp(s, "off") == 0) { *out = false; return 0; }
    return -1;
}

/* ── get ring ────────────────────────────────────────────────────── */

static int net_get_ring(int argc, char **argv, bool json)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int rx = 0, rx_max = 0, tx = 0, tx_max = 0;
    int rc_rx = zenctl_net_get_ring_rx(net, &rx, &err);
    int rc_tx = zenctl_net_get_ring_tx(net, &tx, &err);
    zenctl_net_get_ring_rx_max(net, &rx_max, NULL);
    zenctl_net_get_ring_tx_max(net, &tx_max, NULL);
    zenctl_net_close(net);
    if (rc_rx != 0 && rc_tx != 0) {
        cmd_print_err(json, &err, "cannot read ring parameters");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_int(&f, "rx", rx);
        out_json_field_int(&f, "tx", tx);
        if (rx_max > 0) out_json_field_int(&f, "rx_max", rx_max);
        if (tx_max > 0) out_json_field_int(&f, "tx_max", tx_max);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32];
        out_kv("Interface", iface);
        snprintf(b, sizeof(b), "%d", rx);          out_kv("RX pending", b);
        snprintf(b, sizeof(b), "%d", tx);          out_kv("TX pending", b);
        snprintf(b, sizeof(b), "%d", rx_max);      out_kv("RX max", b);
        snprintf(b, sizeof(b), "%d", tx_max);      out_kv("TX max", b);
    }
    return 0;
}

/* ── set ring rx|tx ──────────────────────────────────────────────── */

static int net_set_ring(int argc, char **argv, bool json, bool dry_run,
                        bool is_tx)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    /* Positional: "rx <N>" or "tx <N>". The "rx"/"tx" word is argv[0]
     * of the get/set target's residual; here we already consumed it
     * in the dispatcher, so argv[0] is N. But our dispatcher passes
     * the whole residual. Let's accept either form. */
    const char *which_word = NULL;
    const char *n_s = NULL;
    int npos = cmd_positional_count(argc, argv);
    if (npos >= 2) {
        which_word = cmd_positional(argc, argv, 0);
        n_s        = cmd_positional(argc, argv, 1);
    } else if (npos == 1) {
        n_s = cmd_positional(argc, argv, 0);
    } else {
        cmd_print_err(json, NULL, "missing ring size");
        return 1;
    }
    /* If the caller already routed us via "set ring rx <N>" we know is_tx. */
    if (which_word) {
        if (strcmp(which_word, "rx") == 0) is_tx = false;
        else if (strcmp(which_word, "tx") == 0) is_tx = true;
        else if (which_word[0] >= '0' && which_word[0] <= '9') {
            /* Only one positional, the size. */
            n_s = which_word;
        } else {
            cmd_print_err(json, NULL, "ring selector must be 'rx' or 'tx'");
            return 1;
        }
    }
    char *e = NULL;
    long n = strtol(n_s, &e, 10);
    if (e == n_s || *e != '\0' || n < 0) {
        cmd_print_err(json, NULL, "ring size must be a non-negative integer");
        return 1;
    }
    const char *which = is_tx ? "tx" : "rx";

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_ring");
            out_json_field_string(&f, "iface", iface);
            out_json_field_string(&f, "which", which);
            out_json_field_int(&f, "size", (int64_t)n);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set ring %s=%ld on %s", which, n, iface);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int rc = is_tx
        ? zenctl_net_set_ring_tx(net, (int)n, &err)
        : zenctl_net_set_ring_rx(net, (int)n, &err);
    zenctl_net_close(net);
    if (rc != 0) {
        cmd_print_err(json, &err, "set ring failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_string(&f, "which", which);
        out_json_field_int(&f, "size", (int64_t)n);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: ring %s=%ld\n", iface, which, n);
    }
    return 0;
}

/* ── get offload ─────────────────────────────────────────────────── */

static int net_get_offload(int argc, char **argv, bool json)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    bool tso = false, gro = false, gso = false, rxc = false, txc = false;
    zenctl_net_get_offload_tso(net, &tso, NULL);
    zenctl_net_get_offload_gro(net, &gro, NULL);
    zenctl_net_get_offload_gso(net, &gso, NULL);
    zenctl_net_get_offload_rxcsum(net, &rxc, NULL);
    zenctl_net_get_offload_txcsum(net, &txc, NULL);
    zenctl_net_close(net);

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_bool(&f, "tso", tso);
        out_json_field_bool(&f, "gro", gro);
        out_json_field_bool(&f, "gso", gso);
        out_json_field_bool(&f, "rxcsum", rxc);
        out_json_field_bool(&f, "txcsum", txc);
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("Interface", iface);
        out_kv("TSO",     tso ? "on" : "off");
        out_kv("GRO",     gro ? "on" : "off");
        out_kv("GSO",     gso ? "on" : "off");
        out_kv("RXCSUM",  rxc ? "on" : "off");
        out_kv("TXCSUM",  txc ? "on" : "off");
    }
    return 0;
}

/* ── set offload <flag> <on|off> ─────────────────────────────────── */

static int net_set_offload(int argc, char **argv, bool json, bool dry_run)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 2) {
        cmd_print_err(json, NULL, "usage: set offload <flag> <on|off>");
        return 1;
    }
    const char *flag = cmd_positional(argc, argv, 0);
    const char *val  = cmd_positional(argc, argv, 1);
    bool on;
    if (parse_on_off(val, &on) != 0) {
        cmd_print_err(json, NULL, "value must be 'on' or 'off'");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_offload");
            out_json_field_string(&f, "iface", iface);
            out_json_field_string(&f, "flag", flag);
            out_json_field_bool(&f, "on", on);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set offload %s=%s on %s", flag, val, iface);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int rc;
    if      (strcmp(flag, "tso")    == 0) rc = zenctl_net_set_offload_tso(net, on, &err);
    else if (strcmp(flag, "gro")    == 0) rc = zenctl_net_set_offload_gro(net, on, &err);
    else if (strcmp(flag, "gso")    == 0) rc = zenctl_net_set_offload_gso(net, on, &err);
    else if (strcmp(flag, "rxcsum") == 0) rc = zenctl_net_set_offload_rxcsum(net, on, &err);
    else if (strcmp(flag, "txcsum") == 0) rc = zenctl_net_set_offload_txcsum(net, on, &err);
    else {
        zenctl_net_close(net);
        cmd_print_err(json, NULL,
                      "unknown offload flag (try tso|gro|gso|rxcsum|txcsum)");
        return 1;
    }
    zenctl_net_close(net);
    if (rc != 0) {
        cmd_print_err(json, &err, "set offload failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_string(&f, "flag", flag);
        out_json_field_bool(&f, "on", on);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: offload %s=%s\n", iface, flag, val);
    }
    return 0;
}

/* ── get link ────────────────────────────────────────────────────── */

static int net_get_link(int argc, char **argv, bool json)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int speed = 0;
    char *duplex = NULL;
    bool up = false;
    int rc_s = zenctl_net_get_speed(net, &speed, NULL);
    int rc_d = zenctl_net_get_duplex(net, &duplex, NULL);
    int rc_l = zenctl_net_get_link(net, &up, NULL);
    zenctl_net_close(net);

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        if (rc_l == 0) out_json_field_bool(&f, "link_up", up);
        if (rc_s == 0) out_json_field_int(&f, "speed_mbps", speed);
        if (rc_d == 0) out_json_field_string(&f, "duplex", duplex ? duplex : "unknown");
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32];
        out_kv("Interface", iface);
        if (rc_l == 0) out_kv("Link", up ? "up" : "down");
        if (rc_s == 0) {
            snprintf(b, sizeof(b), "%d Mbps", speed);
            out_kv("Speed", b);
        }
        if (rc_d == 0) out_kv("Duplex", duplex ? duplex : "unknown");
    }
    free(duplex);
    return 0;
}

/* ── get / set mtu ───────────────────────────────────────────────── */

static int net_get_mtu(int argc, char **argv, bool json)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int mtu = 0;
    int rc = zenctl_net_get_mtu(net, &mtu, &err);
    zenctl_net_close(net);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read MTU");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_int(&f, "mtu", mtu);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[16]; snprintf(b, sizeof(b), "%d", mtu);
        out_kv("Interface", iface);
        out_kv("MTU", b);
    }
    return 0;
}

static int net_set_mtu(int argc, char **argv, bool json, bool dry_run)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing MTU value");
        return 1;
    }
    const char *s = cmd_positional(argc, argv, 0);
    char *e = NULL;
    long mtu = strtol(s, &e, 10);
    if (e == s || *e != '\0' || mtu < 68 || mtu > 65536) {
        cmd_print_err(json, NULL, "MTU must be in 68..65536");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_mtu");
            out_json_field_string(&f, "iface", iface);
            out_json_field_int(&f, "mtu", (int64_t)mtu);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set mtu=%ld on %s", mtu, iface);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int rc = zenctl_net_set_mtu(net, (int)mtu, &err);
    zenctl_net_close(net);
    if (rc != 0) {
        cmd_print_err(json, &err, "set MTU failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_int(&f, "mtu", (int64_t)mtu);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: mtu=%ld\n", iface, mtu);
    }
    return 0;
}

/* ── get irq / set irq-affinity ──────────────────────────────────── */

static int net_get_irq(int argc, char **argv, bool json)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int irq = -1;
    char *mask = NULL;
    int rc_i = zenctl_net_get_irq(net, &irq, &err);
    int rc_a = zenctl_net_get_irq_affinity(net, &mask, NULL);
    zenctl_net_close(net);

    if (rc_i != 0) {
        cmd_print_err(json, &err, "cannot read IRQ");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_int(&f, "irq", irq);
        if (rc_a == 0) out_json_field_string(&f, "smp_affinity", mask ? mask : "");
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32];
        out_kv("Interface", iface);
        snprintf(b, sizeof(b), "%d", irq);
        out_kv("IRQ", b);
        if (rc_a == 0) out_kv("SMP affinity", mask ? mask : "");
    }
    free(mask);
    return 0;
}

static int net_set_irq_affinity(int argc, char **argv, bool json, bool dry_run)
{
    const char *iface = NULL;
    if (get_iface_arg(argc, argv, &iface) <= 0) {
        cmd_print_err(json, NULL, "missing --iface <iface>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing cpumask");
        return 1;
    }
    const char *mask = cmd_positional(argc, argv, 0);

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_irq_affinity");
            out_json_field_string(&f, "iface", iface);
            out_json_field_string(&f, "smp_affinity", mask);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set irq affinity='%s' on %s", mask, iface);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_net_t *net = zenctl_net_open(iface, &err);
    if (!net) {
        cmd_print_err(json, &err, "cannot open interface");
        return 1;
    }
    int rc = zenctl_net_set_irq_affinity(net, mask, &err);
    zenctl_net_close(net);
    if (rc != 0) {
        cmd_print_err(json, &err, "set irq-affinity failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "iface", iface);
        out_json_field_string(&f, "smp_affinity", mask);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: smp_affinity=%s\n", iface, mask);
    }
    return 0;
}

/* ── list ────────────────────────────────────────────────────────── */

static int iface_filter(const char *name)
{
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    return 1;
}

static int net_list(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    char **list = NULL;
    int n = 0;
    if (cmd_list_dir("/sys/class/net", iface_filter, &list, &n) < 0) {
        cmd_print_err(json, NULL, "cannot enumerate /sys/class/net");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "count", n);
        out_json_field_array_begin(&f, "interfaces");
        bool arr_first = true;
        for (int i = 0; i < n; i++)
            out_json_array_string(&arr_first, list[i]);
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "INTERFACE" };
        out_table_header(h, 1);
        for (int i = 0; i < n; i++) {
            const char *row[] = { list[i] };
            out_table_row(row, 1);
        }
    }
    cmd_free_list(list, n);
    return 0;
}

/* ── top-level dispatch ──────────────────────────────────────────── */

int cmd_net(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)confirm;
    if (argc < 1) {
        cmd_print_err(json, NULL, "missing subcommand");
        return 1;
    }
    const char *sub = argv[0];

    if (strcmp(sub, "list") == 0)
        return net_list(argc - 1, argv + 1, json);

    if (strcmp(sub, "get") == 0) {
        if (argc < 2) { cmd_print_err(json, NULL, "missing get target"); return 1; }
        const char *what = argv[1];
        int rargc = argc - 2; char **rargv = argv + 2;
        if (strcmp(what, "ring")    == 0) return net_get_ring(rargc, rargv, json);
        if (strcmp(what, "offload") == 0) return net_get_offload(rargc, rargv, json);
        if (strcmp(what, "link")    == 0) return net_get_link(rargc, rargv, json);
        if (strcmp(what, "mtu")     == 0) return net_get_mtu(rargc, rargv, json);
        if (strcmp(what, "irq")     == 0) return net_get_irq(rargc, rargv, json);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown net get target '%s'. Valid: ring, offload, link, mtu, irq", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }
    if (strcmp(sub, "set") == 0) {
        if (argc < 2) { cmd_print_err(json, NULL, "missing set target"); return 1; }
        const char *what = argv[1];
        int rargc = argc - 2; char **rargv = argv + 2;
        if (strcmp(what, "ring") == 0) {
            /* "set ring rx <N>" or "set ring tx <N>" */
            return net_set_ring(rargc, rargv, json, dry_run, false);
        }
        if (strcmp(what, "offload")       == 0) return net_set_offload(rargc, rargv, json, dry_run);
        if (strcmp(what, "mtu")           == 0) return net_set_mtu(rargc, rargv, json, dry_run);
        if (strcmp(what, "irq-affinity")  == 0) return net_set_irq_affinity(rargc, rargv, json, dry_run);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown net set target '%s'. Valid: ring, offload, mtu, irq-affinity", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "unknown net subcommand '%s'. Try: list, get, set", sub);
    cmd_print_err(json, NULL, buf);
    return 1;
}
