/* test_usb.c - USB domain unit tests against a mock sysfs tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_usb_device(void)
{
    /* Build /sys/bus/usb/devices/1-2 with the attributes the typed
     * API reads. The bus topology files (busnum, devnum) are read at
     * open() time but their absence is tolerated (handle stays valid,
     * only the ioctl-based reset path needs them). */
    mock_sysfs_create_dir("sys/bus/usb/devices/1-2/power");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/idVendor", "1234");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/idProduct", "5678");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/manufacturer",
                           "Acme Widgets");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/product",
                           "USB Test Gadget");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/speed", "480");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/power/control", "auto");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/power/autosuspend_delay_ms",
                           "2000");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/power/runtime_status",
                           "active");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/authorized", "1");
    mock_sysfs_create_file("sys/bus/usb/devices/1-2/power/wakeup",
                           "enabled");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *usb = zenctl_usb_open("1-2", &err);
    OK(usb != NULL, "usb_open(\"1-2\") succeeds against mock tree");
    if (!usb) return;

    int id = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_get_vendor_id(usb, &id, &err) == 0,
       "get_vendor_id returns 0");
    OK(id == 0x1234, "get_vendor_id returns 0x1234");

    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_get_product_id(usb, &id, &err) == 0,
       "get_product_id returns 0");
    OK(id == 0x5678, "get_product_id returns 0x5678");

    char *s = NULL;
    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_get_manufacturer(usb, &s, &err) == 0,
       "get_manufacturer returns 0");
    OK(s && strcmp(s, "Acme Widgets") == 0,
       "get_manufacturer returns \"Acme Widgets\"");
    free(s);

    memset(&err, 0, sizeof(err));
    s = NULL;
    OK(zenctl_usb_get_speed(usb, &s, &err) == 0, "get_speed returns 0");
    OK(s && strcmp(s, "480") == 0, "get_speed returns \"480\"");
    free(s);

    /* power control */
    memset(&err, 0, sizeof(err));
    s = NULL;
    OK(zenctl_usb_get_power_control(usb, &s, &err) == 0,
       "get_power_control returns 0");
    OK(s && strcmp(s, "auto") == 0, "get_power_control returns \"auto\"");
    free(s);

    /* authorized (bool) */
    bool auth = false;
    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_get_authorized(usb, &auth, &err) == 0,
       "get_authorized returns 0");
    OK(auth == true, "get_authorized returns true for \"1\"");

    /* set_authorized writes the canonical string */
    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_set_authorized(usb, false, &err) == 0,
       "set_authorized(false) returns 0");
    char buf[16];
    int n = mock_sysfs_read_file("sys/bus/usb/devices/1-2/authorized",
                                 buf, sizeof(buf));
    OK(n >= 0, "authorized file exists after write");
    OK(strcmp(buf, "0") == 0, "authorized file contains \"0\"");

    /* wakeup (bool, parsed from "enabled" / "disabled") */
    bool wake = false;
    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_get_wakeup(usb, &wake, &err) == 0, "get_wakeup returns 0");
    OK(wake == true, "get_wakeup returns true for \"enabled\"");

    /* set_power_control rejects invalid mode */
    memset(&err, 0, sizeof(err));
    OK(zenctl_usb_set_power_control(usb, "bogus", &err) == -1,
       "set_power_control(\"bogus\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_power_control(\"bogus\") sets ZENCTL_ERR_EINVAL");

    zenctl_usb_close(usb);
}

static void test_usb_open_missing(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *usb = zenctl_usb_open("9-9", &err);
    OK(usb == NULL, "usb_open(\"9-9\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "usb_open(missing) sets ZENCTL_ERR_ENOENT");
}

static void test_usb_open_bad_path(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_usb_t *usb = zenctl_usb_open("1-2:1.0", &err);
    /* ':' is the interface separator, rejected by dev_path_valid */
    OK(usb == NULL, "usb_open(\"1-2:1.0\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "usb_open(\"1-2:1.0\") sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    usb = zenctl_usb_open("", &err);
    OK(usb == NULL, "usb_open(\"\") rejected");
}

int test_usb_suite(void)
{
    SUITE_START("USB domain");
    test_usb_device();
    test_usb_open_missing();
    test_usb_open_bad_path();
    SUITE_END();
    return SUITE_FAILURES();
}
