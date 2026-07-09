/* bt.h - Bluetooth domain API
 *
 * Wraps the Bluetooth adapter sysfs (/sys/class/bluetooth/hci<N>/),
 * the rfkill interface (/sys/class/rfkill/), and the HCI socket
 * ioctls. See docs/KERNEL_USB_BT_FW.md section 2.
 *
 * For v1, power is toggled via rfkill soft-block. The mgmt-socket
 * path (MGMT_OP_SET_POWERED) is documented in the kernel spec but not
 * implemented here; tracked as TODO. Adapter address is read via the
 * HCI socket ioctl HCIGETDEVINFO.
 */
#ifndef ZENCTL_BT_H
#define ZENCTL_BT_H

#include "zenctl.h"

typedef struct zenctl_bt zenctl_bt_t;

/* Open by adapter name, e.g. "hci0". The name is the directory under
 * /sys/class/bluetooth/. Returns NULL and sets *err on failure. */
zenctl_bt_t *zenctl_bt_open(const char *adapter, zenctl_err_t *err);
void          zenctl_bt_close(zenctl_bt_t *bt);

/* Adapter power. Implemented via rfkill soft-block: powered=true
 * means soft-block=0 (radio on), powered=false means soft-block=1
 * (radio off). Returns ZENCTL_ERR_ENOTSUP if no rfkill device of type
 * "bluetooth" matches this adapter. */
int zenctl_bt_get_powered(zenctl_bt_t *bt, bool *out, zenctl_err_t *err);
int zenctl_bt_set_powered(zenctl_bt_t *bt, bool on, zenctl_err_t *err);

/* Direct rfkill access. */
int zenctl_bt_get_rfkill_blocked(zenctl_bt_t *bt, bool *out, zenctl_err_t *err);
int zenctl_bt_set_rfkill_blocked(zenctl_bt_t *bt, bool blocked, zenctl_err_t *err);

/* Adapter BD_ADDR as a "xx:xx:xx:xx:xx:xx" string (heap-allocated,
 * caller frees). Read via the HCIGETDEVINFO ioctl on a raw HCI socket.
 * Returns ZENCTL_ERR_ENOTSUP if the kernel Bluetooth stack is not
 * available. */
int zenctl_bt_get_address(zenctl_bt_t *bt, char **out, zenctl_err_t *err);

/* Enumerate adapter names (hci0, hci1, ...) under
 * /sys/class/bluetooth/. Heap-allocated strings; caller frees each
 * and the array. */
int zenctl_bt_enumerate(char ***out_list, int *out_count, zenctl_err_t *err);

#endif /* ZENCTL_BT_H */
