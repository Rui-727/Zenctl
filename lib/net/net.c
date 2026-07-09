/* net.c - network domain implementation
 *
 * Two kernel interfaces:
 *   - ethtool ioctl via SIOCETHTOOL on an AF_INET SOCK_DGRAM socket,
 *     used for ring params and offload flags.
 *   - sysfs under /sys/class/net/<iface>/, used for speed, duplex,
 *     carrier, mtu, and IRQ number/affinity.
 *
 * String writes are sent verbatim with no trailing newline; numeric
 * writes use zenctl__write_file_i64.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

#include "zenctl/internal.h"
#include "zenctl/net.h"

/* ── Internal helpers ────────────────────────────────────────────── */

#define ZENCTL_SYS_NET   "/sys/class/net"
#define ZENCTL_PROC_IRQ  "/proc/irq"
#define ZENCTL_PATH_MAX  512

struct zenctl_net {
    int  sockfd;            /* AF_INET SOCK_DGRAM, or -1 if open failed */
    char iface[IFNAMSIZ];
};

static int net_validate_iface(const char *iface, zenctl_err_t *err)
{
    size_t n;
    if (!iface || !*iface) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "interface name is required",
                        "zenctl_net_open");
        return -1;
    }
    n = strlen(iface);
    if (n >= IFNAMSIZ) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "interface name too long",
                        "zenctl_net_open");
        return -1;
    }
    if (strchr(iface, '/') || strstr(iface, "..") ||
        strchr(iface, '\n')) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "interface name has invalid characters",
                        "zenctl_net_open");
        return -1;
    }
    return 0;
}

static void net_sysfs_path(char *buf, size_t bufsz, const zenctl_net_t *net,
                           const char *suffix)
{
    snprintf(buf, bufsz, "%s/%s%s", ZENCTL_SYS_NET, net->iface, suffix);
}

/* Run an ethtool ioctl. Returns 0 on success, -1 on error.
 * `data` is the ethtool command struct; its first __u32 must be the
 * command number. */
static int net_ethtool_ioctl(zenctl_net_t *net, void *data, zenctl_err_t *err)
{
    struct ifreq ifr;

    if (net->sockfd < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EIO, "socket not open",
                        "zenctl_net_ethtool_ioctl");
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    /* iface is validated to be < IFNAMSIZ at open time. */
    memcpy(ifr.ifr_name, net->iface, strlen(net->iface) + 1);
    ifr.ifr_data = (void *)data;

    if (ioctl(net->sockfd, SIOCETHTOOL, &ifr) != 0) {
        int e = errno;
        int code;
        switch (e) {
        case EOPNOTSUPP: case ENOSYS:
            code = ZENCTL_ERR_ENOTSUP; break;
        case ENOENT:
            code = ZENCTL_ERR_ENOENT; break;
        case EACCES:
            code = ZENCTL_ERR_EPERM; break;
        case EINVAL:
            code = ZENCTL_ERR_EINVAL; break;
        default:
            code = ZENCTL_ERR_EIO; break;
        }
        zenctl__set_err(err, code, strerror(e), "SIOCETHTOOL");
        return -1;
    }
    return 0;
}

/* Get/set an ethtool_value flag pair. */
static int net_get_flag(zenctl_net_t *net, __u32 cmd, bool *out,
                        zenctl_err_t *err)
{
    struct ethtool_value ev;
    memset(&ev, 0, sizeof(ev));
    ev.cmd = cmd;
    if (net_ethtool_ioctl(net, &ev, err) < 0)
        return -1;
    *out = (ev.data != 0);
    return 0;
}

static int net_set_flag(zenctl_net_t *net, __u32 cmd, bool on,
                        zenctl_err_t *err)
{
    struct ethtool_value ev;
    memset(&ev, 0, sizeof(ev));
    ev.cmd  = cmd;
    ev.data = on ? 1 : 0;
    return net_ethtool_ioctl(net, &ev, err);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

zenctl_net_t *zenctl_net_open(const char *iface, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    zenctl_net_t *net;

    if (net_validate_iface(iface, err) != 0)
        return NULL;

    snprintf(path, sizeof(path), "%s/%s", ZENCTL_SYS_NET, iface);
    if (access(path, F_OK) != 0) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        "interface not found", path);
        return NULL;
    }

    net = calloc(1, sizeof(*net));
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed",
                        "zenctl_net_open");
        return NULL;
    }
    snprintf(net->iface, sizeof(net->iface), "%s", iface);

    net->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (net->sockfd < 0) {
        int e = errno;
        zenctl__set_err(err,
                        (e == EACCES) ? ZENCTL_ERR_EPERM : ZENCTL_ERR_EIO,
                        strerror(e), "socket(AF_INET, SOCK_DGRAM)");
        /* ethtool ops will fail later, but sysfs-only ops still work. */
    }
    return net;
}

void zenctl_net_close(zenctl_net_t *net)
{
    if (!net) return;
    if (net->sockfd >= 0)
        close(net->sockfd);
    free(net);
}

/* ── Ring buffers ────────────────────────────────────────────────── */

static int net_get_ringparams(zenctl_net_t *net,
                              struct ethtool_ringparam *rp,
                              zenctl_err_t *err)
{
    memset(rp, 0, sizeof(*rp));
    rp->cmd = ETHTOOL_GRINGPARAM;
    return net_ethtool_ioctl(net, rp, err);
}

int zenctl_net_get_ring_rx(zenctl_net_t *net, int *out, zenctl_err_t *err)
{
    struct ethtool_ringparam rp;
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_ring_rx");
        return -1;
    }
    if (net_get_ringparams(net, &rp, err) < 0)
        return -1;
    *out = (int)rp.rx_pending;
    return 0;
}

int zenctl_net_get_ring_rx_max(zenctl_net_t *net, int *out, zenctl_err_t *err)
{
    struct ethtool_ringparam rp;
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_ring_rx_max");
        return -1;
    }
    if (net_get_ringparams(net, &rp, err) < 0)
        return -1;
    *out = (int)rp.rx_max_pending;
    return 0;
}

int zenctl_net_set_ring_rx(zenctl_net_t *net, int size, zenctl_err_t *err)
{
    struct ethtool_ringparam rp;
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_ring_rx");
        return -1;
    }
    if (size < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "size must be >= 0",
                        "zenctl_net_set_ring_rx");
        return -1;
    }
    if (net_get_ringparams(net, &rp, err) < 0)
        return -1;
    rp.cmd = ETHTOOL_SRINGPARAM;
    rp.rx_pending = (__u32)size;
    /* Preserve other pending values to avoid resetting them. */
    return net_ethtool_ioctl(net, &rp, err);
}

int zenctl_net_get_ring_tx(zenctl_net_t *net, int *out, zenctl_err_t *err)
{
    struct ethtool_ringparam rp;
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_ring_tx");
        return -1;
    }
    if (net_get_ringparams(net, &rp, err) < 0)
        return -1;
    *out = (int)rp.tx_pending;
    return 0;
}

int zenctl_net_get_ring_tx_max(zenctl_net_t *net, int *out, zenctl_err_t *err)
{
    struct ethtool_ringparam rp;
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_ring_tx_max");
        return -1;
    }
    if (net_get_ringparams(net, &rp, err) < 0)
        return -1;
    *out = (int)rp.tx_max_pending;
    return 0;
}

int zenctl_net_set_ring_tx(zenctl_net_t *net, int size, zenctl_err_t *err)
{
    struct ethtool_ringparam rp;
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_ring_tx");
        return -1;
    }
    if (size < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "size must be >= 0",
                        "zenctl_net_set_ring_tx");
        return -1;
    }
    if (net_get_ringparams(net, &rp, err) < 0)
        return -1;
    rp.cmd = ETHTOOL_SRINGPARAM;
    rp.tx_pending = (__u32)size;
    return net_ethtool_ioctl(net, &rp, err);
}

/* ── Offload flags ───────────────────────────────────────────────── */

int zenctl_net_get_offload_tso(zenctl_net_t *net, bool *out, zenctl_err_t *err)
{
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_offload_tso");
        return -1;
    }
    return net_get_flag(net, ETHTOOL_GTSO, out, err);
}

int zenctl_net_set_offload_tso(zenctl_net_t *net, bool on, zenctl_err_t *err)
{
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_offload_tso");
        return -1;
    }
    return net_set_flag(net, ETHTOOL_STSO, on, err);
}

int zenctl_net_get_offload_gro(zenctl_net_t *net, bool *out, zenctl_err_t *err)
{
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_offload_gro");
        return -1;
    }
    return net_get_flag(net, ETHTOOL_GGRO, out, err);
}

int zenctl_net_set_offload_gro(zenctl_net_t *net, bool on, zenctl_err_t *err)
{
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_offload_gro");
        return -1;
    }
    return net_set_flag(net, ETHTOOL_SGRO, on, err);
}

int zenctl_net_get_offload_gso(zenctl_net_t *net, bool *out, zenctl_err_t *err)
{
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_offload_gso");
        return -1;
    }
    return net_get_flag(net, ETHTOOL_GGSO, out, err);
}

int zenctl_net_set_offload_gso(zenctl_net_t *net, bool on, zenctl_err_t *err)
{
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_offload_gso");
        return -1;
    }
    return net_set_flag(net, ETHTOOL_SGSO, on, err);
}

int zenctl_net_get_offload_rxcsum(zenctl_net_t *net, bool *out, zenctl_err_t *err)
{
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_offload_rxcsum");
        return -1;
    }
    return net_get_flag(net, ETHTOOL_GRXCSUM, out, err);
}

int zenctl_net_set_offload_rxcsum(zenctl_net_t *net, bool on, zenctl_err_t *err)
{
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_offload_rxcsum");
        return -1;
    }
    return net_set_flag(net, ETHTOOL_SRXCSUM, on, err);
}

int zenctl_net_get_offload_txcsum(zenctl_net_t *net, bool *out, zenctl_err_t *err)
{
    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_offload_txcsum");
        return -1;
    }
    return net_get_flag(net, ETHTOOL_GTXCSUM, out, err);
}

int zenctl_net_set_offload_txcsum(zenctl_net_t *net, bool on, zenctl_err_t *err)
{
    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_offload_txcsum");
        return -1;
    }
    return net_set_flag(net, ETHTOOL_STXCSUM, on, err);
}

/* ── Link info (sysfs) ───────────────────────────────────────────── */

int zenctl_net_get_speed(zenctl_net_t *net, int *out_mbps, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!net || !out_mbps) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out_mbps",
                        "zenctl_net_get_speed");
        return -1;
    }
    net_sysfs_path(path, sizeof(path), net, "/speed");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    /* Kernel reports SPEED_UNKNOWN (-1) when link is down or driver
     * doesn't implement get_link_ksettings. Surface as -1. */
    if (v < INT_MIN || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE, "speed out of range", path);
        return -1;
    }
    *out_mbps = (int)v;
    return 0;
}

int zenctl_net_get_duplex(zenctl_net_t *net, char **out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[64];

    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_duplex");
        return -1;
    }
    *out = NULL;
    net_sysfs_path(path, sizeof(path), net, "/duplex");
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) < 0)
        return -1;
    *out = strdup(buf);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

int zenctl_net_get_link(zenctl_net_t *net, bool *up, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!net || !up) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or up",
                        "zenctl_net_get_link");
        return -1;
    }
    net_sysfs_path(path, sizeof(path), net, "/carrier");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    *up = (v != 0);
    return 0;
}

int zenctl_net_get_mtu(zenctl_net_t *net, int *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_mtu");
        return -1;
    }
    net_sysfs_path(path, sizeof(path), net, "/mtu");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    if (v < 0 || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE, "mtu out of range", path);
        return -1;
    }
    *out = (int)v;
    return 0;
}

int zenctl_net_set_mtu(zenctl_net_t *net, int mtu, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!net) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net",
                        "zenctl_net_set_mtu");
        return -1;
    }
    if (mtu < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "mtu must be >= 0",
                        "zenctl_net_set_mtu");
        return -1;
    }
    net_sysfs_path(path, sizeof(path), net, "/mtu");
    return zenctl__write_file_i64(path, (int64_t)mtu, err);
}

/* ── IRQ affinity ────────────────────────────────────────────────── */

int zenctl_net_get_irq(zenctl_net_t *net, int *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!net || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out",
                        "zenctl_net_get_irq");
        return -1;
    }
    /* device is a symlink to the parent bus device (PCI, USB, etc.). */
    net_sysfs_path(path, sizeof(path), net, "/device/irq");
    if (zenctl__read_file_i64(path, &v, err) < 0)
        return -1;
    if (v < 0 || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE, "irq out of range", path);
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int net_irq_path(zenctl_net_t *net, char *buf, size_t bufsz,
                        const char *suffix, zenctl_err_t *err)
{
    char devpath[ZENCTL_PATH_MAX];
    int64_t v;

    net_sysfs_path(devpath, sizeof(devpath), net, "/device/irq");
    if (zenctl__read_file_i64(devpath, &v, err) < 0)
        return -1;
    if (v < 0 || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE, "irq out of range", devpath);
        return -1;
    }
    snprintf(buf, bufsz, "%s/%lld%s", ZENCTL_PROC_IRQ, (long long)v, suffix);
    return 0;
}

int zenctl_net_get_irq_affinity(zenctl_net_t *net, char **out_cpumask,
                                zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[256];

    if (!net || !out_cpumask) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or out_cpumask",
                        "zenctl_net_get_irq_affinity");
        return -1;
    }
    *out_cpumask = NULL;
    if (net_irq_path(net, path, sizeof(path), "/smp_affinity", err) < 0)
        return -1;
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) < 0)
        return -1;
    *out_cpumask = strdup(buf);
    if (!*out_cpumask) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

int zenctl_net_set_irq_affinity(zenctl_net_t *net, const char *cpumask,
                                zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!net || !cpumask || !*cpumask) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL net or cpumask",
                        "zenctl_net_set_irq_affinity");
        return -1;
    }
    if (strchr(cpumask, '\n') || strlen(cpumask) >= 256) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "cpumask too long or contains newline",
                        "zenctl_net_set_irq_affinity");
        return -1;
    }
    if (net_irq_path(net, path, sizeof(path), "/smp_affinity", err) < 0)
        return -1;
    return zenctl__write_file_string(path, cpumask, err);
}
