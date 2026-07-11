/* cmd_pcie.c - PCIe domain commands
 *
 * Subcommands:
 *   get  link | power | iommu | numa | info | aspm
 *   set  power
 *   list
 *
 * `list` scans /sys/bus/pci/devices/ directly (read-only directory
 * listing); the library does not yet expose PCI enumeration.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/pcie.h"
#include "output.h"
#include "cmd_util.h"

/* ── helpers ─────────────────────────────────────────────────────── */

static int get_addr_arg(int argc, char **argv, const char **out)
{
    return cmd_opt_str(argc, argv, "--addr", out);
}

/* ── get link ────────────────────────────────────────────────────── */

static int pcie_get_link(int argc, char **argv, bool json)
{
    const char *addr = NULL;
    if (get_addr_arg(argc, argv, &addr) <= 0) {
        cmd_print_err(json, NULL, "missing --addr <pci-addr>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_pcie_t *p = zenctl_pcie_open(addr, &err);
    if (!p) {
        cmd_print_err(json, &err, "cannot open PCI device");
        return 1;
    }
    char *speed = NULL, *max_speed = NULL;
    int width = 0, max_width = 0;
    int rc_s = zenctl_pcie_get_link_speed(p, &speed, &err);
    int rc_w = zenctl_pcie_get_link_width(p, &width, &err);
    zenctl_pcie_get_max_link_speed(p, &max_speed, NULL);
    zenctl_pcie_get_max_link_width(p, &max_width, NULL);
    zenctl_pcie_close(p);
    if (rc_s != 0 && rc_w != 0) {
        cmd_print_err(json, &err, "cannot read link info");
        free(speed); free(max_speed);
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "address", addr);
        if (rc_s == 0) out_json_field_string(&f, "current_link_speed", speed);
        if (rc_w == 0) out_json_field_int(&f, "current_link_width", width);
        if (max_speed) out_json_field_string(&f, "max_link_speed", max_speed);
        out_json_field_int(&f, "max_link_width", max_width);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32];
        out_kv("Address", addr);
        if (rc_s == 0) out_kv("Current link speed", speed);
        if (rc_w == 0) {
            snprintf(b, sizeof(b), "x%d", width);
            out_kv("Current link width", b);
        }
        if (max_speed) out_kv("Max link speed", max_speed);
        snprintf(b, sizeof(b), "x%d", max_width);
        out_kv("Max link width", b);
    }
    free(speed); free(max_speed);
    return 0;
}

/* ── get / set power ─────────────────────────────────────────────── */

static int pcie_get_power(int argc, char **argv, bool json)
{
    const char *addr = NULL;
    if (get_addr_arg(argc, argv, &addr) <= 0) {
        cmd_print_err(json, NULL, "missing --addr <pci-addr>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_pcie_t *p = zenctl_pcie_open(addr, &err);
    if (!p) {
        cmd_print_err(json, &err, "cannot open PCI device");
        return 1;
    }
    char *mode = NULL;
    bool d3cold = false;
    int rc_p = zenctl_pcie_get_power_control(p, &mode, &err);
    int rc_d = zenctl_pcie_get_d3cold_allowed(p, &d3cold, NULL);
    zenctl_pcie_close(p);
    if (rc_p != 0) {
        cmd_print_err(json, &err, "cannot read power control");
        free(mode);
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "address", addr);
        out_json_field_string(&f, "power", mode);
        if (rc_d == 0) out_json_field_bool(&f, "d3cold_allowed", d3cold);
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("Address", addr);
        out_kv("Power control", mode);
        if (rc_d == 0) out_kv("D3cold allowed", d3cold ? "yes" : "no");
    }
    free(mode);
    return 0;
}

static int pcie_set_power(int argc, char **argv, bool json, bool dry_run)
{
    const char *addr = NULL;
    if (get_addr_arg(argc, argv, &addr) <= 0) {
        cmd_print_err(json, NULL, "missing --addr <pci-addr>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing power mode (auto|on)");
        return 1;
    }
    const char *mode = cmd_positional(argc, argv, 0);
    if (strcmp(mode, "auto") != 0 && strcmp(mode, "on") != 0) {
        cmd_print_err(json, NULL, "power mode must be 'auto' or 'on'");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_power");
            out_json_field_string(&f, "address", addr);
            out_json_field_string(&f, "power", mode);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set power='%s' on %s", mode, addr);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_pcie_t *p = zenctl_pcie_open(addr, &err);
    if (!p) {
        cmd_print_err(json, &err, "cannot open PCI device");
        return 1;
    }
    int rc = zenctl_pcie_set_power_control(p, mode, &err);
    zenctl_pcie_close(p);
    if (rc != 0) {
        cmd_print_err(json, &err, "set power failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "address", addr);
        out_json_field_string(&f, "power", mode);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: power=%s\n", addr, mode);
    }
    return 0;
}

/* ── get iommu ───────────────────────────────────────────────────── */

static int pcie_get_iommu(int argc, char **argv, bool json)
{
    const char *addr = NULL;
    if (get_addr_arg(argc, argv, &addr) <= 0) {
        cmd_print_err(json, NULL, "missing --addr <pci-addr>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_pcie_t *p = zenctl_pcie_open(addr, &err);
    if (!p) {
        cmd_print_err(json, &err, "cannot open PCI device");
        return 1;
    }
    int group = -1;
    int rc = zenctl_pcie_get_iommu_group(p, &group, &err);
    zenctl_pcie_close(p);
    if (rc != 0) {
        /* ENOENT means no IOMMU group; report 0 gracefully. */
        if (err.code == ZENCTL_ERR_ENOENT) {
            if (json) {
                out_json_ok_begin();
                out_json_open_object();
                bool f = true;
                out_json_field_string(&f, "address", addr);
                out_json_field_int(&f, "iommu_group", -1);
                out_json_field_bool(&f, "has_iommu", false);
                out_json_close_object();
                out_json_ok_end();
            } else {
                out_kv("Address", addr);
                out_kv("IOMMU group", "none");
            }
            return 0;
        }
        cmd_print_err(json, &err, "cannot read IOMMU group");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "address", addr);
        out_json_field_int(&f, "iommu_group", group);
        out_json_field_bool(&f, "has_iommu", true);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[16]; snprintf(b, sizeof(b), "%d", group);
        out_kv("Address", addr);
        out_kv("IOMMU group", b);
    }
    return 0;
}

/* ── get numa ────────────────────────────────────────────────────── */

static int pcie_get_numa(int argc, char **argv, bool json)
{
    const char *addr = NULL;
    if (get_addr_arg(argc, argv, &addr) <= 0) {
        cmd_print_err(json, NULL, "missing --addr <pci-addr>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_pcie_t *p = zenctl_pcie_open(addr, &err);
    if (!p) {
        cmd_print_err(json, &err, "cannot open PCI device");
        return 1;
    }
    int node = -1;
    int rc = zenctl_pcie_get_numa_node(p, &node, &err);
    zenctl_pcie_close(p);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read NUMA node");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "address", addr);
        out_json_field_int(&f, "numa_node", node);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32];
        if (node < 0) snprintf(b, sizeof(b), "unknown (%d)", node);
        else          snprintf(b, sizeof(b), "%d", node);
        out_kv("Address", addr);
        out_kv("NUMA node", b);
    }
    return 0;
}

/* ── get info ────────────────────────────────────────────────────── */

static int pcie_get_info(int argc, char **argv, bool json)
{
    const char *addr = NULL;
    if (get_addr_arg(argc, argv, &addr) <= 0) {
        cmd_print_err(json, NULL, "missing --addr <pci-addr>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_pcie_t *p = zenctl_pcie_open(addr, &err);
    if (!p) {
        cmd_print_err(json, &err, "cannot open PCI device");
        return 1;
    }
    int vendor = 0, device = 0, class = 0;
    int rc_v = zenctl_pcie_get_vendor_id(p, &vendor, &err);
    int rc_d = zenctl_pcie_get_device_id(p, &device, &err);
    int rc_c = zenctl_pcie_get_class(p, &class, &err);
    zenctl_pcie_close(p);
    if (rc_v != 0) {
        cmd_print_err(json, &err, "cannot read vendor id");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "address", addr);
        out_json_field_int(&f, "vendor_id", vendor);
        out_json_field_int(&f, "device_id", device);
        out_json_field_int(&f, "class", class);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32];
        out_kv("Address", addr);
        snprintf(b, sizeof(b), "0x%04x", vendor);  out_kv("Vendor ID", b);
        snprintf(b, sizeof(b), "0x%04x", device);  out_kv("Device ID", b);
        snprintf(b, sizeof(b), "0x%06x", class);   out_kv("Class", b);
        (void)rc_d; (void)rc_c;
    }
    return 0;
}

/* ── get aspm ────────────────────────────────────────────────────── */

static int pcie_get_aspm(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    zenctl_err_t err;
    char *policy = NULL;
    if (zenctl_pcie_get_aspm_policy(&policy, &err) != 0) {
        cmd_print_err(json, &err, "cannot read ASPM policy");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "aspm_policy", policy);
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("ASPM policy", policy);
    }
    free(policy);
    return 0;
}

/* ── list ────────────────────────────────────────────────────────── */

static int pcie_dev_filter(const char *name)
{
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    /* PCI device dirs look like DDDD:BB:DD.F or BBB:DD.F or BB:DD.F */
    if (!strchr(name, ':')) return 0;
    if (!strchr(name, '.')) return 0;
    return 1;
}

static int pcie_list(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    char **list = NULL;
    int n = 0;
    if (cmd_list_dir("/sys/bus/pci/devices", pcie_dev_filter, &list, &n) < 0) {
        cmd_print_err(json, NULL, "cannot enumerate /sys/bus/pci/devices");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "count", n);
        out_json_field_array_begin(&f, "devices");
        bool arr_first = true;
        for (int i = 0; i < n; i++)
            out_json_array_string(&arr_first, list[i]);
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "ADDRESS" };
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

int cmd_pcie(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)confirm;
    if (argc < 1) {
        cmd_print_err(json, NULL, "missing subcommand");
        return 1;
    }
    const char *sub = argv[0];

    if (strcmp(sub, "list") == 0)
        return pcie_list(argc - 1, argv + 1, json);

    if (strcmp(sub, "get") == 0) {
        if (argc < 2) { cmd_print_err(json, NULL, "missing get target"); return 1; }
        const char *what = argv[1];
        int rargc = argc - 2; char **rargv = argv + 2;
        if (strcmp(what, "link")  == 0) return pcie_get_link(rargc, rargv, json);
        if (strcmp(what, "power") == 0) return pcie_get_power(rargc, rargv, json);
        if (strcmp(what, "iommu") == 0) return pcie_get_iommu(rargc, rargv, json);
        if (strcmp(what, "numa")  == 0) return pcie_get_numa(rargc, rargv, json);
        if (strcmp(what, "info")  == 0) return pcie_get_info(rargc, rargv, json);
        if (strcmp(what, "aspm")  == 0) return pcie_get_aspm(rargc, rargv, json);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown pcie get target '%s'. Valid: link, power, iommu, numa, info, aspm", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }
    if (strcmp(sub, "set") == 0) {
        if (argc < 2) { cmd_print_err(json, NULL, "missing set target"); return 1; }
        const char *what = argv[1];
        int rargc = argc - 2; char **rargv = argv + 2;
        if (strcmp(what, "power") == 0) return pcie_set_power(rargc, rargv, json, dry_run);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown pcie set target '%s'. Valid: power", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "unknown pcie subcommand '%s'. Try: list, get, set", sub);
    cmd_print_err(json, NULL, buf);
    return 1;
}
