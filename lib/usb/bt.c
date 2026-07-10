/* bt.c - Bluetooth domain implementation.
 *
 * Power control uses the HCI socket ioctls (HCIDEVUP / HCIDEVDOWN /
 * HCIGETDEVINFO) via lib/usb/bt_mgmt.c as the primary path, with
 * rfkill soft-block as a fallback when the HCI ioctls fail (e.g.
 * EPERM on a system where the user lacks CAP_NET_ADMIN but can write
 * /sys/class/rfkill/rfkillN/soft). The adapter address is read via
 * the HCIGETDEVINFO ioctl on a raw HCI socket.
 *
 * Because Bluetooth development headers (<net/bluetooth/hci.h>) may
 * not be installed on every build host, the HCI protocol constants
 * and structures used here are defined locally. They are stable
 * kernel ABI (include/uapi/linux/) and identical across versions.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "zenctl/internal.h"
#include "zenctl/bt.h"

#include "rfkill.h"
#include "util.h"
#include "bt_mgmt.h"

#define BT_SYSFS_BASE "/sys/class/bluetooth"

/* Bluetooth protocol constant (include/uapi/linux/socket.h). */
#ifndef BTPROTO_HCI
#define BTPROTO_HCI 1
#endif

/* HCI socket ioctl (include/uapi/linux/bluetooth/hci_sock.h).
 * _IOR encodes the size of the third argument; the kernel ignores it
 * for pointer-based ioctls, so int works. */
#ifndef HCIGETDEVINFO
#define HCIGETDEVINFO _IOR('H', 211, int)
#endif

typedef struct {
    uint8_t b[6];
} __attribute__((packed)) bt_bdaddr_t;

/* struct hci_dev_info from include/uapi/linux/bluetooth/hci_dev.h.
 * Layout must match the kernel's exactly. */
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
} __attribute__((packed)) bt_hci_dev_stats;

typedef struct {
    uint16_t          dev_id;
    char              name[8];
    bt_bdaddr_t       bdaddr;
    uint32_t          flags;
    uint8_t           type;
    uint8_t           features[8];
    uint32_t          pkt_type;
    uint32_t          link_policy;
    uint32_t          link_mode;
    uint16_t          acl_mtu;
    uint16_t          acl_pkts;
    uint16_t          sco_mtu;
    uint16_t          sco_pkts;
    bt_hci_dev_stats  stat;
} __attribute__((packed)) bt_hci_dev_info;

struct zenctl_bt {
    char *adapter;     /* e.g. "hci0" */
    char *sysfs_dir;   /* /sys/class/bluetooth/hci0 */
    int   index;       /* adapter index parsed from name, -1 if unknown */
};

static bool adapter_name_valid(const char *p)
{
    if (!p || strncmp(p, "hci", 3) != 0) return false;
    if (p[3] == '\0') return false;
    for (const char *c = p + 3; *c; c++)
        if (!isdigit((unsigned char)*c)) return false;
    return true;
}

zenctl_bt_t *zenctl_bt_open(const char *adapter, zenctl_err_t *err)
{
    if (!adapter_name_valid(adapter)) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "zenctl_bt_open(%s)",
                 adapter ? adapter : "NULL");
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "adapter must be 'hci<N>'", ctx);
        return NULL;
    }
    zenctl_bt_t *bt = calloc(1, sizeof(*bt));
    if (!bt) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed", "zenctl_bt_open");
        return NULL;
    }
    bt->adapter = strdup(adapter);
    if (!bt->adapter) {
        free(bt);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", "zenctl_bt_open");
        return NULL;
    }
    bt->sysfs_dir = zenctl_util_path_join(BT_SYSFS_BASE, adapter);
    if (!bt->sysfs_dir) {
        free(bt->adapter); free(bt);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", "zenctl_bt_open");
        return NULL;
    }
    bt->index = atoi(adapter + 3);

    DIR *d = opendir(bt->sysfs_dir);
    if (!d) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), bt->sysfs_dir);
        free(bt->sysfs_dir);
        free(bt->adapter);
        free(bt);
        return NULL;
    }
    closedir(d);
    return bt;
}

void zenctl_bt_close(zenctl_bt_t *bt)
{
    if (!bt) return;
    free(bt->sysfs_dir);
    free(bt->adapter);
    free(bt);
}

int zenctl_bt_get_rfkill_blocked(zenctl_bt_t *bt, bool *out, zenctl_err_t *err)
{
    if (!bt || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL bt or out", "zenctl_bt_get_rfkill_blocked");
        return -1;
    }
    int idx = zenctl_rfkill_find(bt->adapter, "bluetooth", err);
    if (idx < 0) return -1;
    return zenctl_rfkill_get_soft(idx, out, err);
}

int zenctl_bt_set_rfkill_blocked(zenctl_bt_t *bt, bool blocked, zenctl_err_t *err)
{
    if (!bt) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL bt", "zenctl_bt_set_rfkill_blocked");
        return -1;
    }
    int idx = zenctl_rfkill_find(bt->adapter, "bluetooth", err);
    if (idx < 0) return -1;
    return zenctl_rfkill_set_soft(idx, blocked, err);
}

int zenctl_bt_get_powered(zenctl_bt_t *bt, bool *out, zenctl_err_t *err)
{
    if (!bt || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL bt or out", "zenctl_bt_get_powered");
        return -1;
    }
    /* Primary path: HCI socket HCIGETDEVINFO. The "powered" state is
     * the HCI_UP flag in struct hci_dev_info.flags. */
    int fd = bt_mgmt_open();
    if (fd >= 0) {
        int rc = bt_mgmt_get_powered(fd, bt->index);
        int saved_errno = errno;
        close(fd);
        if (rc >= 0) {
            *out = (rc == 1);
            return 0;
        }
        /* Fall through to rfkill on EPERM / ENOENT / ENODEV (adapter
         * exists in sysfs but not registered with HCI core, or the
         * user lacks CAP_NET_ADMIN). Other errors (EBADF, EINVAL)
         * mean the caller passed bad arguments; surface them. */
        if (saved_errno != EPERM && saved_errno != EACCES &&
            saved_errno != ENOENT && saved_errno != ENODEV) {
            zenctl__set_err(err, zenctl__errno_to_code(saved_errno),
                            strerror(saved_errno),
                            "bt_mgmt_get_powered");
            return -1;
        }
    }
    /* Fallback: rfkill soft-block. powered = !soft_blocked. */
    bool blocked = false;
    int rc = zenctl_bt_get_rfkill_blocked(bt, &blocked, err);
    if (rc != 0) return rc;
    *out = !blocked;
    return 0;
}

int zenctl_bt_set_powered(zenctl_bt_t *bt, bool on, zenctl_err_t *err)
{
    if (!bt) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL bt", "zenctl_bt_set_powered");
        return -1;
    }
    /* Primary path: HCIDEVUP / HCIDEVDOWN. */
    int fd = bt_mgmt_open();
    if (fd >= 0) {
        int rc = bt_mgmt_set_powered(fd, bt->index, on);
        int saved_errno = errno;
        close(fd);
        if (rc == 0) return 0;
        /* EPERM/EACCES: caller lacks CAP_NET_ADMIN, try rfkill. */
        if (saved_errno != EPERM && saved_errno != EACCES) {
            zenctl__set_err(err, zenctl__errno_to_code(saved_errno),
                            strerror(saved_errno),
                            "bt_mgmt_set_powered");
            return -1;
        }
    }
    /* Fallback: rfkill soft-block. powered=true -> soft=0. */
    return zenctl_bt_set_rfkill_blocked(bt, !on, err);
}

int zenctl_bt_get_address(zenctl_bt_t *bt, char **out, zenctl_err_t *err)
{
    if (!bt || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL bt or out", "zenctl_bt_get_address");
        return -1;
    }
    if (bt->index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "adapter index unknown", "zenctl_bt_get_address");
        return -1;
    }

    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) {
        int e = errno;
        int code = zenctl__errno_to_code(e);
        /* If the kernel lacks Bluetooth support, surface ENOTSUP. */
        if (e == EAFNOSUPPORT || e == EPROTONOSUPPORT || e == EINVAL)
            code = ZENCTL_ERR_ENOTSUP;
        zenctl__set_err(err, code, strerror(e), "socket(AF_BLUETOOTH, BTPROTO_HCI)");
        return -1;
    }

    bt_hci_dev_info info;
    memset(&info, 0, sizeof(info));
    info.dev_id = (uint16_t)bt->index;
    if (ioctl(fd, HCIGETDEVINFO, &info) < 0) {
        int e = errno;
        close(fd);
        zenctl__set_err(err, zenctl__errno_to_code(e), strerror(e), "HCIGETDEVINFO");
        return -1;
    }
    close(fd);

    /* All-zero address means the adapter is not powered up or the
     * driver hasn't supplied one. Surface ENOTSUP in that case so the
     * caller can power the adapter up first. */
    bool all_zero = (info.bdaddr.b[0] | info.bdaddr.b[1] | info.bdaddr.b[2] |
                     info.bdaddr.b[3] | info.bdaddr.b[4] | info.bdaddr.b[5]) == 0;
    if (all_zero) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "zenctl_bt_get_address(%s)", bt->adapter);
        zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                        "adapter has no BD_ADDR (power it on first)", ctx);
        return -1;
    }

    char *s = malloc(32);
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "malloc failed", "zenctl_bt_get_address");
        return -1;
    }
    snprintf(s, 32, "%02x:%02x:%02x:%02x:%02x:%02x",
             info.bdaddr.b[5], info.bdaddr.b[4], info.bdaddr.b[3],
             info.bdaddr.b[2], info.bdaddr.b[1], info.bdaddr.b[0]);
    *out = s;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

int zenctl_bt_enumerate(char ***out_list, int *out_count, zenctl_err_t *err)
{
    if (!out_list || !out_count) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out_list or out_count",
                        "zenctl_bt_enumerate");
        return -1;
    }
    char **entries = zenctl_util_list_dir(BT_SYSFS_BASE, err);
    if (!entries) return -1;

    size_t cap = 8, n = 0;
    char **out = calloc(cap, sizeof(char *));
    if (!out) {
        for (size_t i = 0; entries[i]; i++) free(entries[i]);
        free(entries);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", "zenctl_bt_enumerate");
        return -1;
    }
    for (size_t i = 0; entries[i]; i++) {
        if (!adapter_name_valid(entries[i])) continue;
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **narr = realloc(out, ncap * sizeof(char *));
            if (!narr) {
                for (size_t j = 0; j < n; j++) free(out[j]);
                free(out);
                for (size_t j = 0; entries[j]; j++) free(entries[j]);
                free(entries);
                zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", "zenctl_bt_enumerate");
                return -1;
            }
            out = narr; cap = ncap;
        }
        out[n] = entries[i];
        entries[i] = NULL;
        n++;
    }
    for (size_t i = 0; entries[i]; i++) free(entries[i]);
    free(entries);

    qsort(out, n, sizeof(char *), cmp_str);

    *out_list = out;
    *out_count = (int)n;
    return 0;
}
