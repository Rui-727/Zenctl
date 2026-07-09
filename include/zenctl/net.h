/* net.h - network domain API
 *
 * NIC controls: RX/TX ring sizes, offload flags (TSO/GRO/GSO/RXcsum/
 * TXcsum), link info (speed/duplex/carrier/mtu), and IRQ affinity.
 * Offload and ring ops use the SIOCETHTOOL ioctl; link info uses
 * /sys/class/net/<iface>/ sysfs files.
 */
#ifndef ZENCTL_NET_H
#define ZENCTL_NET_H

#include "zenctl.h"

typedef struct zenctl_net zenctl_net_t;

/* Open by interface name (e.g. "eth0", "enp3s0", "wlp2s0").
 * Returns NULL on error and fills *err. */
zenctl_net_t *zenctl_net_open(const char *iface, zenctl_err_t *err);
void          zenctl_net_close(zenctl_net_t *net);

/* Ring buffer sizes (descriptors). Via ethtool GRINGPARAM / SRINGPARAM.
 * - *_max: hardware maximum (read-only).
 * - set_*: writes the current pending count. */
int zenctl_net_get_ring_rx(zenctl_net_t *net, int *out, zenctl_err_t *err);
int zenctl_net_get_ring_rx_max(zenctl_net_t *net, int *out, zenctl_err_t *err);
int zenctl_net_set_ring_rx(zenctl_net_t *net, int size, zenctl_err_t *err);
int zenctl_net_get_ring_tx(zenctl_net_t *net, int *out, zenctl_err_t *err);
int zenctl_net_get_ring_tx_max(zenctl_net_t *net, int *out, zenctl_err_t *err);
int zenctl_net_set_ring_tx(zenctl_net_t *net, int size, zenctl_err_t *err);

/* Offload flags. Each uses an ethtool_value ioctl pair
 * (e.g. ETHTOOL_GTSO / ETHTOOL_STSO). */
int zenctl_net_get_offload_tso(zenctl_net_t *net, bool *out, zenctl_err_t *err);
int zenctl_net_set_offload_tso(zenctl_net_t *net, bool on, zenctl_err_t *err);
int zenctl_net_get_offload_gro(zenctl_net_t *net, bool *out, zenctl_err_t *err);
int zenctl_net_set_offload_gro(zenctl_net_t *net, bool on, zenctl_err_t *err);
int zenctl_net_get_offload_gso(zenctl_net_t *net, bool *out, zenctl_err_t *err);
int zenctl_net_set_offload_gso(zenctl_net_t *net, bool on, zenctl_err_t *err);
int zenctl_net_get_offload_rxcsum(zenctl_net_t *net, bool *out, zenctl_err_t *err);
int zenctl_net_set_offload_rxcsum(zenctl_net_t *net, bool on, zenctl_err_t *err);
int zenctl_net_get_offload_txcsum(zenctl_net_t *net, bool *out, zenctl_err_t *err);
int zenctl_net_set_offload_txcsum(zenctl_net_t *net, bool on, zenctl_err_t *err);

/* Link info via sysfs. */
int zenctl_net_get_speed(zenctl_net_t *net, int *out_mbps, zenctl_err_t *err);
int zenctl_net_get_duplex(zenctl_net_t *net, char **out, zenctl_err_t *err);
int zenctl_net_get_link(zenctl_net_t *net, bool *up, zenctl_err_t *err);
int zenctl_net_get_mtu(zenctl_net_t *net, int *out, zenctl_err_t *err);
int zenctl_net_set_mtu(zenctl_net_t *net, int mtu, zenctl_err_t *err);

/* IRQ affinity. get_irq reads /sys/class/net/<iface>/device/irq.
 * Affinity get/set use /proc/irq/<N>/smp_affinity (hex cpumask).
 * Note: for MSI-X NICs with multiple vectors this only addresses
 * the legacy/first IRQ — a known limitation of the simple API. */
int zenctl_net_get_irq(zenctl_net_t *net, int *out, zenctl_err_t *err);
int zenctl_net_get_irq_affinity(zenctl_net_t *net, char **out_cpumask, zenctl_err_t *err);
int zenctl_net_set_irq_affinity(zenctl_net_t *net, const char *cpumask, zenctl_err_t *err);

#endif /* ZENCTL_NET_H */
