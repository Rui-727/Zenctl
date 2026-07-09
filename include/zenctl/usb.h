/* usb.h - USB domain API
 *
 * Wraps the sysfs interface under /sys/bus/usb/devices/ and the
 * USBDEVFS ioctls on /dev/bus/usb/<bus>/<dev>. See
 * docs/KERNEL_USB_BT_FW.md section 1 for the kernel ABI reference.
 */
#ifndef ZENCTL_USB_H
#define ZENCTL_USB_H

#include "zenctl.h"

typedef struct zenctl_usb zenctl_usb_t;

/* Open by USB device path, e.g. "1-2" or "2-1.3". The path is the
 * directory name under /sys/bus/usb/devices/. Returns NULL and sets
 * *err on failure. */
zenctl_usb_t *zenctl_usb_open(const char *dev_path, zenctl_err_t *err);
void          zenctl_usb_close(zenctl_usb_t *usb);

/* Device info. idVendor / idProduct are returned as ints parsed from
 * the 4-hex-digit sysfs strings. manufacturer / product / speed are
 * returned as heap-allocated strings the caller must free(). speed is
 * the raw sysfs string (e.g. "480", "5000", "0"); callers wanting a
 * pretty form should format it themselves. */
int zenctl_usb_get_vendor_id(zenctl_usb_t *usb, int *out, zenctl_err_t *err);
int zenctl_usb_get_product_id(zenctl_usb_t *usb, int *out, zenctl_err_t *err);
int zenctl_usb_get_manufacturer(zenctl_usb_t *usb, char **out, zenctl_err_t *err);
int zenctl_usb_get_product(zenctl_usb_t *usb, char **out, zenctl_err_t *err);
int zenctl_usb_get_speed(zenctl_usb_t *usb, char **out, zenctl_err_t *err);

/* Power management. control is "auto" or "on". autosuspend_delay_ms
 * is signed (negative = never suspend). runtime_status is "active",
 * "suspended", "unsupported", or "suspended". */
int zenctl_usb_get_power_control(zenctl_usb_t *usb, char **out, zenctl_err_t *err);
int zenctl_usb_set_power_control(zenctl_usb_t *usb, const char *mode, zenctl_err_t *err);
int zenctl_usb_get_autosuspend_delay(zenctl_usb_t *usb, int *out_ms, zenctl_err_t *err);
int zenctl_usb_set_autosuspend_delay(zenctl_usb_t *usb, int ms, zenctl_err_t *err);
int zenctl_usb_get_runtime_status(zenctl_usb_t *usb, char **out, zenctl_err_t *err);

/* Enable/disable device. Reads / writes the "authorized" sysfs file. */
int zenctl_usb_get_authorized(zenctl_usb_t *usb, bool *out, zenctl_err_t *err);
int zenctl_usb_set_authorized(zenctl_usb_t *usb, bool auth, zenctl_err_t *err);

/* Wakeup. The power/wakeup file is empty when the device has no
 * remote-wakeup capability; in that case get returns false. */
int zenctl_usb_get_wakeup(zenctl_usb_t *usb, bool *out, zenctl_err_t *err);
int zenctl_usb_set_wakeup(zenctl_usb_t *usb, bool on, zenctl_err_t *err);

/* Reset via USBDEVFS_RESET ioctl on /dev/bus/usb/<bus>/<dev>. Requires
 * write access to the usbfs character device (root or udev-granted
 * group). The device keeps its address and configuration but all
 * endpoint state is cleared. */
int zenctl_usb_reset(zenctl_usb_t *usb, zenctl_err_t *err);

/* Enumerate all USB device paths under /sys/bus/usb/devices/. The
 * returned list contains heap-allocated strings the caller must free
 * individually (and the array itself via free()). Only entries that
 * look like USB device paths (N-P or usbN) are returned; USB
 * interface entries (N-P:C.I) are skipped. On success returns 0; on
 * failure returns -1 and sets *err. */
int zenctl_usb_enumerate(char ***out_list, int *out_count, zenctl_err_t *err);

#endif /* ZENCTL_USB_H */
