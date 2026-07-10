/* bt_mgmt.c - Bluetooth HCI socket ioctls for power control.
 *
 * Uses the legacy HCI socket ioctls (HCIDEVUP / HCIDEVDOWN /
 * HCIGETDEVINFO), the same surface `hciconfig hci0 up/down` uses.
 * More direct than the mgmt socket (no BlueZ daemon, no mgmt
 * command/event framing) and works on any kernel with the bluetooth
 * module loaded.
 *
 * When libbluetooth-dev is available at build time (HAVE_BLUETOOTH
 * defined by the Makefile), the system headers supply the protocol
 * constants and struct layouts. Otherwise we provide minimal local
 * definitions of the kernel ABI (stable since 2.6, identical across
 * versions) so the build succeeds without the optional dependency.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bt_mgmt.h"

#ifdef HAVE_BLUETOOTH
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* HCI_UP is a bit index from <bluetooth/hci.h>; normalise to a mask. */
#define HCI_UP_BIT (1u << HCI_UP)
/* Alias our local type name to the system struct. */
typedef struct hci_dev_info bt_mgmt_dev_info;

#else
/* Bluetooth protocol family (include/uapi/linux/socket.h). */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef BTPROTO_HCI
#define BTPROTO_HCI 1
#endif

/* HCI socket ioctls (include/uapi/linux/bluetooth/hci_sock.h).
 * _IO encodes no argument; _IOR encodes a read direction with a
 * size hint the kernel ignores for pointer ioctls. */
#ifndef HCIDEVUP
#define HCIDEVUP    _IO('H', 201)
#endif
#ifndef HCIDEVDOWN
#define HCIDEVDOWN  _IO('H', 202)
#endif
#ifndef HCIGETDEVINFO
#define HCIGETDEVINFO _IOR('H', 211, int)
#endif

/* Bit 0 of struct hci_dev_info.flags; the adapter is "up" (powered,
 * initialised) when set. */
#define HCI_UP_BIT (1u << 0)

/* struct hci_dev_info (kernel ABI). Layout must match the kernel's
 * exactly so the ioctl's sizeof matches. */
typedef struct {
    uint8_t b[6];
} __attribute__((packed)) bt_mgmt_bdaddr_t;

typedef struct {
    uint32_t err_rx;
    uint32_t err_tx;
    uint32_t cmd_tx;
    uint32_t evt_rx;
    uint32_t acl_tx;
    uint32_t acl_rx;
    uint32_t sco_tx;
    uint32_t sco_rx;
    uint32_t byte_rx;
    uint32_t byte_tx;
} __attribute__((packed)) bt_mgmt_dev_stats;

typedef struct {
    uint16_t            dev_id;
    char                name[8];
    bt_mgmt_bdaddr_t    bdaddr;
    uint32_t            flags;
    uint8_t             type;
    uint8_t             features[8];
    uint32_t            pkt_type;
    uint32_t            link_policy;
    uint32_t            link_mode;
    uint16_t            acl_mtu;
    uint16_t            acl_pkts;
    uint16_t            sco_mtu;
    uint16_t            sco_pkts;
    bt_mgmt_dev_stats   stat;
} __attribute__((packed)) bt_mgmt_dev_info;
#endif /* HAVE_BLUETOOTH */

int bt_mgmt_open(void)
{
    return socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
}

int bt_mgmt_hci_to_index(const char *name)
{
    if (!name) return -1;
    if (strncmp(name, "hci", 3) != 0) return -1;
    if (name[3] == '\0') return -1;
    int idx = 0;
    for (const char *p = name + 3; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        idx = idx * 10 + (*p - '0');
        /* dev_id is a uint16_t in the kernel ABI. */
        if (idx > 65535) return -1;
    }
    return idx;
}

int bt_mgmt_get_powered(int fd, int adapter_idx)
{
    if (fd < 0) { errno = EBADF; return -1; }
    if (adapter_idx < 0 || adapter_idx > 65535) { errno = EINVAL; return -1; }

    bt_mgmt_dev_info info;
    memset(&info, 0, sizeof(info));
    info.dev_id = (uint16_t)adapter_idx;
    if (ioctl(fd, HCIGETDEVINFO, &info) < 0) return -1;
    return (info.flags & HCI_UP_BIT) ? 1 : 0;
}

int bt_mgmt_set_powered(int fd, int adapter_idx, bool on)
{
    if (fd < 0) { errno = EBADF; return -1; }
    if (adapter_idx < 0 || adapter_idx > 65535) { errno = EINVAL; return -1; }

    int req = on ? HCIDEVUP : HCIDEVDOWN;
    if (ioctl(fd, req, adapter_idx) < 0) {
        /* HCIDEVUP returns EALREADY when the adapter is already up;
         * HCIDEVDOWN returns EALREADY when already down. Treat both
         * as success so set_powered() is idempotent. */
        if (errno == EALREADY) return 0;
        return -1;
    }
    return 0;
}
