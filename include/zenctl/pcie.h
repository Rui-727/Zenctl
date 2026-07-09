/* pcie.h - PCIe / IOMMU domain API
 *
 * Per-device PCIe link speed/width, ASPM global policy, runtime PM
 * (D3cold), IOMMU group, NUMA node, and config-space identification.
 * Backed by /sys/bus/pci/devices/<addr>/ and
 * /sys/module/pcie_aspm/parameters/policy.
 */
#ifndef ZENCTL_PCIE_H
#define ZENCTL_PCIE_H

#include "zenctl.h"

typedef struct zenctl_pcie zenctl_pcie_t;

/* Open by PCI address, e.g. "0000:01:00.0". Returns NULL on error. */
zenctl_pcie_t *zenctl_pcie_open(const char *pci_addr, zenctl_err_t *err);
void           zenctl_pcie_close(zenctl_pcie_t *pcie);

/* Link speed and width. Speed strings come from the kernel and look
 * like "2.5 GT/s", "8.0 GT/s", "16.0 GT/s", "32.0 GT/s", "64.0 GT/s".
 * Width is a decimal lane count (1, 2, 4, 8, 16, 32). */
int zenctl_pcie_get_link_speed(zenctl_pcie_t *pcie, char **out, zenctl_err_t *err);
int zenctl_pcie_get_link_width(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err);
int zenctl_pcie_get_max_link_speed(zenctl_pcie_t *pcie, char **out, zenctl_err_t *err);
int zenctl_pcie_get_max_link_width(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err);

/* ASPM. Global policy read from /sys/module/pcie_aspm/parameters/policy.
 * The returned string is the raw sysfs contents (active entry bracketed). */
int zenctl_pcie_get_aspm_policy(char **out, zenctl_err_t *err);

/* Power management. /sys/bus/pci/devices/<addr>/power/control and
 * /sys/bus/pci/devices/<addr>/d3cold_allowed. */
int zenctl_pcie_get_power_control(zenctl_pcie_t *pcie, char **out, zenctl_err_t *err);
int zenctl_pcie_set_power_control(zenctl_pcie_t *pcie, const char *mode, zenctl_err_t *err);
int zenctl_pcie_get_d3cold_allowed(zenctl_pcie_t *pcie, bool *out, zenctl_err_t *err);
int zenctl_pcie_set_d3cold_allowed(zenctl_pcie_t *pcie, bool allow, zenctl_err_t *err);

/* IOMMU group ID. Reads the iommu_group symlink and extracts <N>
 * from /sys/kernel/iommu_groups/<N>. Returns -ENOENT if the device
 * has no IOMMU group. */
int zenctl_pcie_get_iommu_group(zenctl_pcie_t *pcie, int *out_group_id, zenctl_err_t *err);

/* NUMA node. /sys/bus/pci/devices/<addr>/numa_node. -1 = unknown. */
int zenctl_pcie_get_numa_node(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err);

/* Device identification from config space.
 * vendor, device, class are returned as raw integers (e.g. vendor
 * 0x8086, device 0x1528, class 0x020000). */
int zenctl_pcie_get_vendor_id(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err);
int zenctl_pcie_get_device_id(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err);
int zenctl_pcie_get_class(zenctl_pcie_t *pcie, int *out, zenctl_err_t *err);

#endif /* ZENCTL_PCIE_H */
