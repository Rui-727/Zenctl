/* main.c - zenctl CLI entry point
 *
 * Parses `zenctl <domain> <subcommand> [target] [options]` and
 * dispatches to per-domain handlers. Global flags (--json, --dry-run,
 * --confirm, --help, --version) are extracted from argv anywhere they
 * appear and passed to handlers as booleans; the remaining args go to
 * the handler with argv[0] = subcommand.
 *
 * Root check: any subcommand whose verb starts with "set" (or one of
 * the profile write verbs save/load/delete) requires root. Read-only
 * ops work unprivileged.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "output.h"
#include "cmd.h"

#define ZENCTL_VERSION "0.1.0"

/* ── Dispatch table ──────────────────────────────────────────────── */

static const struct {
    const char    *domain;
    cmd_handler_fn fn;
} dispatch_table[] = {
    {"cpu",       cmd_cpu},
    {"mem",       cmd_mem},
    {"storage",   cmd_storage},
    {"net",       cmd_net},
    {"pcie",      cmd_pcie},
    {"gpu",       cmd_gpu},
    {"thermal",   cmd_thermal},
    {"power",     cmd_power},
    {"usb",       cmd_usb},
    {"bt",        cmd_bt},
    {"wireless",  cmd_wireless},
    {"firmware",  cmd_firmware},
    {"profile",   cmd_profile},
    {"caps",      cmd_caps},
    {NULL, NULL},
};

/* ── Help / version ──────────────────────────────────────────────── */

static void print_version(void)
{
    printf("zenctl %s\n", ZENCTL_VERSION);
}

static void print_help(void)
{
    printf(
"Usage: zenctl <domain> <subcommand> [target] [options]\n"
"       zenctl profile <save|load|list|delete> <name>\n"
"       zenctl caps [domain]\n"
"       zenctl --version | --help\n"
"\n"
"zenctl is a unified Linux hardware control surface.\n"
"\n"
"Domains:\n"
"  cpu       CPU frequency, governor, online, SMT, C-states\n"
"  mem       hugepages, THP, NUMA, swappiness, overcommit\n"
"  storage   block-device scheduler, queue depth, read-ahead, cache\n"
"  net       NIC rings, offloads, link, MTU, IRQ affinity\n"
"  pcie      PCIe link, power, ASPM, IOMMU, NUMA\n"
"  gpu       GPU temp, fan, power, frequency, AMDGPU profiles\n"
"  thermal   thermal zones, trips, cooling, hwmon sensors, fan PWM\n"
"  power     battery, sleep states, charge thresholds, AC, wake\n"
"  usb       USB power, autosuspend, authorize, reset, wakeup\n"
"  bt        Bluetooth power, address, rfkill\n"
"  wireless  Wi-Fi power, regdomain, TX power, power save\n"
"  firmware  DMI, EFI vars, ACPI tables, BIOS settings\n"
"  profile   save/load named hardware profiles (TOML)\n"
"  caps      show capability matrix\n"
"\n"
"Global options:\n"
"  --json        Emit JSON instead of a human-readable table\n"
"  --dry-run     Print what would change, touch nothing\n"
"  --confirm     Skip confirmation prompts for destructive ops\n"
"  -h, --help    Show this help\n"
"  -V, --version Show version\n"
"\n"
"Examples:\n"
"  zenctl cpu get governor --cpu 0\n"
"  zenctl cpu set governor performance --cpu all\n"
"  zenctl cpu set freq-max 3.6GHz --cpu all\n"
"  zenctl storage list --json\n"
"  zenctl net get ring --iface eth0\n"
"  zenctl pcie get link --addr 0000:01:00.0\n"
    );
}

/* ── Global flag filtering ───────────────────────────────────────── */

/* Scan argv[0..argc) and pull out --json, --dry-run, --confirm,
 * --help/-h, --version/-V. Returns a newly-allocated argv-like array
 * (caller frees the array, not the strings) of the remaining args.
 * *out_argc is the count. */
static char **filter_global_flags(int argc, char **argv,
                                  bool *pjson, bool *pdry_run,
                                  bool *pconfirm, bool *phelp,
                                  bool *pversion, int *out_argc)
{
    char **out = calloc(argc > 0 ? argc : 1, sizeof(char *));
    if (!out) {
        *out_argc = 0;
        return NULL;
    }
    int n = 0;
    *pjson = *pdry_run = *pconfirm = *phelp = *pversion = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0)        { *pjson = true;    continue; }
        if (strcmp(a, "--dry-run") == 0)     { *pdry_run = true; continue; }
        if (strcmp(a, "--confirm") == 0 ||
            strcmp(a, "--yes") == 0 ||
            strcmp(a, "-y") == 0)            { *pconfirm = true; continue; }
        if (strcmp(a, "--help") == 0 ||
            strcmp(a, "-h") == 0)            { *phelp = true;    continue; }
        if (strcmp(a, "--version") == 0 ||
            strcmp(a, "-V") == 0)            { *pversion = true; continue; }
        out[n++] = (char *)a;
    }
    *out_argc = n;
    return out;
}

/* ── Dispatch helpers ────────────────────────────────────────────── */

static cmd_handler_fn find_handler(const char *domain)
{
    for (int i = 0; dispatch_table[i].domain; i++) {
        if (strcmp(dispatch_table[i].domain, domain) == 0)
            return dispatch_table[i].fn;
    }
    return NULL;
}

/* Heuristic: which subcommand verbs require root. The convention is
 * that any subcommand starting with "set" is a write op, plus the
 * profile save/load/delete verbs. */
static bool subcommand_needs_root(const char *sub)
{
    if (!sub) return false;
    if (strncmp(sub, "set", 3) == 0) return true;
    if (strcmp(sub, "save")   == 0)  return true;
    if (strcmp(sub, "delete") == 0)  return true;
    if (strcmp(sub, "load")   == 0)  return true;
    return false;
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    bool json = false, dry_run = false, confirm = false;
    bool help = false, version = false;
    int new_argc = 0;

    char **new_argv = filter_global_flags(argc - 1, argv + 1,
                                          &json, &dry_run, &confirm,
                                          &help, &version, &new_argc);
    if (!new_argv) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    if (version) {
        print_version();
        free(new_argv);
        return 0;
    }
    if (help || new_argc == 0) {
        print_help();
        free(new_argv);
        return 0;
    }

    /* new_argv[0] = domain, new_argv[1] = subcommand, ... */
    const char *domain     = new_argv[0];
    const char *subcommand = (new_argc >= 2) ? new_argv[1] : NULL;

    cmd_handler_fn fn = find_handler(domain);
    if (!fn) {
        char buf[256];
        snprintf(buf, sizeof(buf), "unknown domain '%s'", domain);
        if (json)
            out_json_error(3, buf);
        else {
            out_err(buf);
            fprintf(stderr, "Run 'zenctl --help' for a list of domains.\n");
        }
        free(new_argv);
        return 1;
    }

    /* Root check for write ops. --dry-run is a preview that touches
     * nothing, so we let it through unprivileged. */
    if (!dry_run && subcommand_needs_root(subcommand) && getuid() != 0) {
        const char *msg = "operation requires root (run as root or via sudo)";
        if (json)
            out_json_error(1, msg);
        else
            out_err(msg);
        free(new_argv);
        return 1;
    }

    /* Handler sees argv[0] = subcommand. */
    int rc = fn(new_argc - 1, new_argv + 1, json, dry_run, confirm);
    free(new_argv);
    return rc == 0 ? 0 : 1;
}
