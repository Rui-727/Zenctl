/* bt_mgmt.h - private Bluetooth HCI ioctl helpers for power control.
 *
 * Implements the HCIDEVUP / HCIDEVDOWN / HCIGETDEVINFO ioctls on a
 * raw HCI socket. Equivalent to `hciconfig hci0 up/down`. Simpler
 * and more direct than the mgmt socket (no BlueZ daemon required),
 * works on any kernel with the bluetooth module loaded.
 *
 * Not part of the public API; consumed only by lib/usb/bt.c.
 */
#ifndef ZENCTL_USB_BT_MGMT_H
#define ZENCTL_USB_BT_MGMT_H

#include <stdbool.h>

/* Open a raw HCI socket suitable for HCIDEVUP / HCIDEVDOWN /
 * HCIGETDEVINFO. Returns fd >= 0 on success, -1 on error (errno set,
 * typically EAFNOSUPPORT / EPROTONOSUPPORT on a kernel without
 * Bluetooth). */
int bt_mgmt_open(void);

/* Parse an adapter name like "hci0" into its index. Returns the
 * index (>= 0) on success, -1 on a malformed name. */
int bt_mgmt_hci_to_index(const char *name);

/* Read the powered state of the adapter. Returns 1 (on), 0 (off),
 * or -1 on error (errno set). On means the HCI_UP flag is set in
 * the kernel's struct hci_dev_info.flags. */
int bt_mgmt_get_powered(int fd, int adapter_idx);

/* Set the powered state. Returns 0 on success, -1 on error (errno
 * set). HCIDEVUP / HCIDEVDOWN return EALREADY when the adapter is
 * already in the requested state; we treat that as success so
 * callers can use set_powered() idempotently. */
int bt_mgmt_set_powered(int fd, int adapter_idx, bool on);

#endif /* ZENCTL_USB_BT_MGMT_H */
