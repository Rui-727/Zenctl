/* test_pcie.c - PCIe domain unit tests against a mock sysfs tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_pcie_device(void)
{
    mock_sysfs_create_dir("sys/bus/pci/devices/0000:01:00.0");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/current_link_speed",
        "8.0 GT/s");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/current_link_width",
        "16");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/max_link_speed",
        "16.0 GT/s");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/max_link_width",
        "16");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/numa_node", "0");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/vendor", "0x10de");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/device", "0x2204");
    mock_sysfs_create_file(
        "sys/bus/pci/devices/0000:01:00.0/class", "0x030200");
    mock_sysfs_create_dir(
        "sys/bus/pci/devices/0000:01:00.0/power");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_pcie_t *pcie = zenctl_pcie_open("0000:01:00.0", &err);
    OK(pcie != NULL, "pcie_open(\"0000:01:00.0\") succeeds against mock tree");
    if (!pcie) return;

    /* link speed (string) */
    char *s = NULL;
    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_get_link_speed(pcie, &s, &err) == 0,
       "get_link_speed returns 0");
    OK(s && strcmp(s, "8.0 GT/s") == 0,
       "get_link_speed returns \"8.0 GT/s\"");
    free(s);

    /* link width (int) */
    int w = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_get_link_width(pcie, &w, &err) == 0,
       "get_link_width returns 0");
    OK(w == 16, "get_link_width returns 16");

    /* numa node */
    int node = -999;
    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_get_numa_node(pcie, &node, &err) == 0,
       "get_numa_node returns 0");
    OK(node == 0, "get_numa_node returns 0");

    /* vendor / device / class */
    int id = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_get_vendor_id(pcie, &id, &err) == 0,
       "get_vendor_id returns 0");
    OK(id == 0x10de, "get_vendor_id returns 0x10de (NVIDIA)");

    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_get_device_id(pcie, &id, &err) == 0,
       "get_device_id returns 0");
    OK(id == 0x2204, "get_device_id returns 0x2204");

    memset(&err, 0, sizeof(err));
    OK(zenctl_pcie_get_class(pcie, &id, &err) == 0,
       "get_class returns 0");
    OK(id == 0x030200, "get_class returns 0x030200");

    /* IOMMU group: spec says "use a real dir, not symlink, for mock".
     * The library readlink()s the iommu_group entry; on a real dir it
     * gets EINVAL and returns ZENCTL_ERR_ENOENT. */
    mock_sysfs_create_dir(
        "sys/bus/pci/devices/0000:01:00.0/iommu_group");
    int gid = -1;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_pcie_get_iommu_group(pcie, &gid, &err);
    OK(rc == -1, "get_iommu_group returns -1 (iommu_group is a dir, not symlink)");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "get_iommu_group sets ZENCTL_ERR_ENOENT when entry is a directory");

    /* Now model a real IOMMU group symlink. The kernel exposes
     * /sys/bus/pci/devices/<addr>/iommu_group ->
     *   /sys/kernel/iommu_groups/<N>
     * The library readlink()s and extracts <N> from the basename. */
    mock_sysfs_remove("sys/bus/pci/devices/0000:01:00.0/iommu_group");
    mock_sysfs_create_symlink(
        "sys/bus/pci/devices/0000:01:00.0/iommu_group",
        "../../../kernel/iommu_groups/3");
    gid = -1;
    memset(&err, 0, sizeof(err));
    rc = zenctl_pcie_get_iommu_group(pcie, &gid, &err);
    OK(rc == 0, "get_iommu_group returns 0 (symlinked)");
    OK(gid == 3, "get_iommu_group returns 3");

    zenctl_pcie_close(pcie);
}

static void test_pcie_open_missing(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_pcie_t *pcie = zenctl_pcie_open("0000:02:00.0", &err);
    OK(pcie == NULL, "pcie_open(\"0000:02:00.0\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "pcie_open(missing) sets ZENCTL_ERR_ENOENT");
}

static void test_pcie_open_bad_addr(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_pcie_t *pcie = zenctl_pcie_open("not-an-address", &err);
    OK(pcie == NULL, "pcie_open(\"not-an-address\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "pcie_open(\"not-an-address\") sets ZENCTL_ERR_EINVAL");

    /* path traversal */
    memset(&err, 0, sizeof(err));
    pcie = zenctl_pcie_open("../etc/passwd", &err);
    OK(pcie == NULL, "pcie_open(\"../etc/passwd\") rejected");
}

static void test_pcie_aspm_policy(void)
{
    /* /sys/module/pcie_aspm/parameters/policy is mode 0644: the mock
     * fixture creates it writable, so set_aspm_policy should succeed. */
    mock_sysfs_create_dir("sys/module/pcie_aspm/parameters");
    mock_sysfs_create_file("sys/module/pcie_aspm/parameters/policy",
                           "[default] performance powersave powersupersave");

    /* setter: write a valid policy */
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_pcie_set_aspm_policy("performance", &err);
    OK(rc == 0, "set_aspm_policy(\"performance\") returns 0");

    char buf[256];
    int n = mock_sysfs_read_file("sys/module/pcie_aspm/parameters/policy",
                                 buf, sizeof(buf));
    OK(n >= 0, "policy file exists after set_aspm_policy write");
    OK(strcmp(buf, "performance") == 0,
       "policy file contains \"performance\" after set_aspm_policy");

    /* setter: each accepted policy name */
    const char *const names[] = {
        "default", "performance", "powersave", "powersupersave"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        memset(&err, 0, sizeof(err));
        rc = zenctl_pcie_set_aspm_policy(names[i], &err);
        OK(rc == 0, "set_aspm_policy(accepted name) returns 0");
    }

    /* setter: reject invalid policy */
    memset(&err, 0, sizeof(err));
    rc = zenctl_pcie_set_aspm_policy("bogus", &err);
    OK(rc == -1, "set_aspm_policy(\"bogus\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_aspm_policy(invalid) sets ZENCTL_ERR_EINVAL");

    /* setter: reject NULL / empty */
    memset(&err, 0, sizeof(err));
    rc = zenctl_pcie_set_aspm_policy(NULL, &err);
    OK(rc == -1, "set_aspm_policy(NULL) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_aspm_policy(NULL) sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    rc = zenctl_pcie_set_aspm_policy("", &err);
    OK(rc == -1, "set_aspm_policy(\"\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_aspm_policy(\"\") sets ZENCTL_ERR_EINVAL");

    /* getter still works (reads the file we last wrote) */
    char *s = NULL;
    memset(&err, 0, sizeof(err));
    rc = zenctl_pcie_get_aspm_policy(&s, &err);
    OK(rc == 0, "get_aspm_policy returns 0");
    OK(s && strcmp(s, "powersupersave") == 0,
       "get_aspm_policy returns last-written \"powersupersave\"");
    free(s);
}

int test_pcie_suite(void)
{
    SUITE_START("PCIe domain");
    test_pcie_device();
    test_pcie_open_missing();
    test_pcie_open_bad_addr();
    test_pcie_aspm_policy();
    SUITE_END();
    return SUITE_FAILURES();
}
