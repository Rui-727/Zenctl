/* pcie.c - PCIe / IOMMU domain implementation
 *
 * All controls are sysfs-backed under /sys/bus/pci/devices/<addr>/.
 * The PCI address is validated to reject path traversal.
 *
 * Vendor/device/class files are formatted as `0x%04x\n` / `0x%06x\n`
 * by the kernel — we parse them with strtoll base 0 so the 0x prefix
 * is honoured. NUMA node, link width, d3cold_allowed, etc. are plain
 * base-10 and use zenctl__read_file_i64.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>

#include "zenctl/internal.h"
#include "zenctl/pcie.h"

/* ── Internal helpers ────────────────────────────────────────────── */

#define ZENCTL_PCI_DEVICES  "/sys/bus/pci/devices"
#define ZENCTL_ASPM_POLICY  "/sys/module/pcie_aspm/parameters/policy"
#define ZENCTL_PATH_MAX     512

struct zenctl_pcie {
    char pci_addr[32];        /* e.g. "0000:01:00.0" */
};

/* Validate a PCI address: DDDD:BB:DD.F form (or shorter BBB:DD.F or
 * BB:DD.F as the kernel accepts in some places). Reject anything
 * containing slashes, dots-dots, or non-ASCII. */
static int pcie_validate_addr(const char *addr, zenctl_err_t *err)
{
    size_t n, i;
    if (!addr || !*addr) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "PCI address is required",
                        "zenctl_pcie_open");
        return -1;
    }
    n = strlen(addr);
    if (n >= 32) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "PCI address too long",
                        "zenctl_pcie_open");
        return -1;
    }
    for (i = 0; i < n; i++) {
        char c = addr[i];
        if (!(isxdigit((unsigned char)c) || c == ':' || c == '.')) {
            zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                            "PCI address has invalid characters",
                            "zenctl_pcie_open");
            return -1;
        }
    }
    return 0;
}

static void pcie_dev_path(char *buf, size_t bufsz, const zenctl_pcie_t *pcie,
                          const char *suffix)
{
    snprintf(buf, bufsz, "%s/%s%s",
             ZENCTL_PCI_DEVICES, pcie->pci_addr, suffix);
}

/* Read a sysfs integer that may be in base 16 with 0x prefix (vendor,
 * device, class files use 0x%04x / 0x%06x format). */
static int pcie_read_hex_i64(const char *path, int64_t *out, zenctl_err_t *err)
{
    char buf[64];
    char *end;
    long long v;

    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return -1;
    errno = 0;
    v = strtoll(buf, &end, 0);   /* auto-detect base from 0x prefix */
    if (errno != 0 || end == buf) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "not a hex integer", path);
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

zenctl_pcie_t *zenctl_pcie_open(const char *pci_addr, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    zenctl_pcie_t *pcie;

    if (pcie_validate_addr(pci_addr, err) != 0)
        return NULL;

    snprintf(path, sizeof(path), "%s/%s",
             ZENCTL_PCI_DEVICES, pci_addr);
    if (access(path, F_OK) != 0) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        "PCI device not found", path);
        return NULL;
    }

    pcie = calloc(1, sizeof(*pcie));
    if (!pcie) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed",
                        "zenctl_pcie_open");
        return NULL;
    }
    snprintf(pcie->pci_addr, sizeof(pcie->pci_addr), "%s", pci_addr);
    return pcie;
}

void zenctl_pcie_close(zenctl_pcie_t *pcie)
{
    free(pcie);
}

/* ── Link speed and width ────────────────────────────────────────── */

int zenctl_pcie_get_link_speed(zenctl_pcie_t *pcie, char **out,
                               zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[64];

    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_link_speed");
        return -1;
    }
    *out = NULL;
    pcie_dev_path(path, sizeof(path), pcie, "/current_link_speed");
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) < 0)
        return -1;
    *out = strdup(buf);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

int zenctl_pcie_get_link_width(zenctl_pcie_t *pcie, int *out,
                               zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_link_width");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/current_link_width");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    if (v < 0 || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "link width out of range", path);
        return -1;
    }
    *out = (int)v;
    return 0;
}

int zenctl_pcie_get_max_link_speed(zenctl_pcie_t *pcie, char **out,
                                   zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[64];

    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_max_link_speed");
        return -1;
    }
    *out = NULL;
    pcie_dev_path(path, sizeof(path), pcie, "/max_link_speed");
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) < 0)
        return -1;
    *out = strdup(buf);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

int zenctl_pcie_get_max_link_width(zenctl_pcie_t *pcie, int *out,
                                   zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_max_link_width");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/max_link_width");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    if (v < 0 || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "max link width out of range", path);
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* ── ASPM global policy ──────────────────────────────────────────── */

int zenctl_pcie_get_aspm_policy(char **out, zenctl_err_t *err)
{
    char buf[256];

    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out",
                        "zenctl_pcie_get_aspm_policy");
        return -1;
    }
    *out = NULL;
    if (zenctl__read_file_string(ZENCTL_ASPM_POLICY, buf, sizeof(buf),
                                 err) < 0)
        return -1;
    *out = strdup(buf);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed",
                        ZENCTL_ASPM_POLICY);
        return -1;
    }
    return 0;
}

/* ── Power management ────────────────────────────────────────────── */

int zenctl_pcie_get_power_control(zenctl_pcie_t *pcie, char **out,
                                  zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[64];

    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_power_control");
        return -1;
    }
    *out = NULL;
    pcie_dev_path(path, sizeof(path), pcie, "/power/control");
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) < 0)
        return -1;
    *out = strdup(buf);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

int zenctl_pcie_set_power_control(zenctl_pcie_t *pcie, const char *mode,
                                  zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!pcie || !mode || !*mode) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or mode",
                        "zenctl_pcie_set_power_control");
        return -1;
    }
    if (strcmp(mode, "on") != 0 && strcmp(mode, "auto") != 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "mode must be 'on' or 'auto'",
                        "zenctl_pcie_set_power_control");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/power/control");
    return zenctl__write_file_string(path, mode, err);
}

int zenctl_pcie_get_d3cold_allowed(zenctl_pcie_t *pcie, bool *out,
                                   zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_d3cold_allowed");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/d3cold_allowed");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    *out = (v != 0);
    return 0;
}

int zenctl_pcie_set_d3cold_allowed(zenctl_pcie_t *pcie, bool allow,
                                   zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!pcie) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie",
                        "zenctl_pcie_set_d3cold_allowed");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/d3cold_allowed");
    return zenctl__write_file_i64(path, allow ? 1 : 0, err);
}

/* ── IOMMU group ───────────────────────────────────────────────────
 *
 * The kernel exposes /sys/bus/pci/devices/<addr>/iommu_group as a
 * symlink to /sys/kernel/iommu_groups/<N>. We readlink() it and
 * extract the trailing <N> from the basename.
 */
int zenctl_pcie_get_iommu_group(zenctl_pcie_t *pcie, int *out_group_id,
                                zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char linkbuf[ZENCTL_PATH_MAX];
    ssize_t n;
    char *base, *end;
    long long gid;

    if (!pcie || !out_group_id) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out_group_id",
                        "zenctl_pcie_get_iommu_group");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/iommu_group");

    n = readlink(path, linkbuf, sizeof(linkbuf) - 1);
    if (n < 0) {
        int e = errno;
        if (e == EINVAL || e == ENOENT) {
            /* EINVAL = not a symlink (file doesn't model a group);
             * ENOENT = no iommu_group at all (passthrough or no IOMMU). */
            zenctl__set_err(err, ZENCTL_ERR_ENOENT,
                            "device has no IOMMU group", path);
        } else {
            zenctl__set_err(err, zenctl__errno_to_code(e),
                            strerror(e), path);
        }
        return -1;
    }
    linkbuf[n] = '\0';

    /* Strip trailing slash (readlink doesn't add one but be safe). */
    while (n > 0 && linkbuf[n - 1] == '/') {
        linkbuf[--n] = '\0';
    }
    base = strrchr(linkbuf, '/');
    base = base ? base + 1 : linkbuf;
    if (!*base) {
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "cannot parse IOMMU group from symlink", path);
        return -1;
    }
    errno = 0;
    gid = strtoll(base, &end, 10);
    if (errno != 0 || end == base || *end != '\0') {
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "IOMMU group ID is not numeric", path);
        return -1;
    }
    if (gid < INT_MIN || gid > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "IOMMU group ID out of range", path);
        return -1;
    }
    *out_group_id = (int)gid;
    return 0;
}

/* ── NUMA node ───────────────────────────────────────────────────── */

int zenctl_pcie_get_numa_node(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_numa_node");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/numa_node");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    if (v < INT_MIN || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "numa_node out of range", path);
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* ── Device ID ───────────────────────────────────────────────────── */

int zenctl_pcie_get_vendor_id(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;
    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_vendor_id");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/vendor");
    if (pcie_read_hex_i64(path, &v, err) < 0)
        return -1;
    *out = (int)v;
    return 0;
}

int zenctl_pcie_get_device_id(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;
    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_device_id");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/device");
    if (pcie_read_hex_i64(path, &v, err) < 0)
        return -1;
    *out = (int)v;
    return 0;
}

int zenctl_pcie_get_class(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;
    if (!pcie || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL pcie or out",
                        "zenctl_pcie_get_class");
        return -1;
    }
    pcie_dev_path(path, sizeof(path), pcie, "/class");
    if (pcie_read_hex_i64(path, &v, err) < 0)
        return -1;
    /* class is 24-bit (0x00xxxxxx); fits in int easily. */
    *out = (int)v;
    return 0;
}
