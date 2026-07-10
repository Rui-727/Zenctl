/* nl80211.h - private nl80211 (genetlink) helpers for the wireless module.
 *
 * Raw netlink, no libnl dependency. The high-level functions open an
 * AF_NETLINK / SOCK_RAW / NETLINK_GENERIC socket, resolve the nl80211
 * genl family ID via the genl controller (CTRL_CMD_GETFAMILY), and
 * issue NL80211_CMD_* requests. On failure they return -1 and set
 * errno; the wireless.c wrappers translate errno to a zenctl_err_t.
 *
 * The build/parse helpers (double-underscore prefix) are exposed for
 * unit testing — they have no side effects and take a buffer + the
 * family ID as a parameter so tests can verify message construction
 * without opening a socket.
 *
 * See docs/KERNEL_USB_BT_FW.md section 3.2 for the kernel-side spec.
 */
#ifndef ZENCTL_USB_NL80211_H
#define ZENCTL_USB_NL80211_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "zenctl/zenctl.h"

/* ---- high-level API (open a socket, talk to the kernel) ---- */

/* Resolve the nl80211 genl family ID. Returns >= 0 on success, -1 on
 * error (errno set). The ID is cached per-process; subsequent calls
 * do not re-hit the kernel. errno=ESRCH means the nl80211 family is
 * not registered (no cfg80211 in the kernel). */
int nl80211_get_family_id(void);

/* Get the current regulatory domain (2-char alpha2). Writes a
 * NUL-terminated 2-char code into out_alpha2 (which must hold at
 * least 3 bytes). Returns 0 on success, -1 on error. */
int nl80211_get_regdomain(char *out_alpha2, size_t sz);

/* Set the regulatory domain. alpha2 must be a 2-char ISO 3166-1
 * alpha-2 code. Requires CAP_NET_ADMIN. Returns 0 / -1. */
int nl80211_set_regdomain(const char *alpha2);

/* Get TX power for a phy (mBm = 100 * dBm). Returns 0 on success.
 * errno=ENOENT if the kernel doesn't report TX power for this phy
 * (some drivers omit the attribute). */
int nl80211_get_txpower(int phy_idx, int32_t *out_mBm);

/* Set TX power for a phy. mBm is signed; pass -1 for "automatic"
 * (driver picks). Otherwise NL80211_TX_POWER_FIXED is used.
 * Requires CAP_NET_ADMIN. Returns 0 / -1. */
int nl80211_set_txpower(int phy_idx, int32_t mBm);

/* Set 802.11 power save for an interface (not a phy). Requires
 * CAP_NET_ADMIN. Returns 0 / -1. errno=ENOENT if ifindex is invalid. */
int nl80211_set_power_save(int ifindex, bool enabled);

/* Get the interface index (ifindex) for a phy name (e.g. "phy0").
 * Walks /sys/class/net/<iface>/phy80211 symlinks. Returns >= 0 on
 * success, -1 on error (errno=ENOENT if no interface is bound to
 * this phy). */
int nl80211_phy_to_ifindex(const char *phy_name);

/* ---- message construction helpers (exposed for unit testing) ----
 * Each builds a complete netlink message (nlmsghdr + genlmsghdr +
 * attributes) into buf. Returns the total message size on success,
 * -1 on overflow / bad args. The family ID is taken as a parameter
 * so tests can pass a known value without opening a socket. */

/* Build CTRL_CMD_GETFAMILY("nl80211"). Sent to GENL_ID_CTRL. */
ssize_t nl80211__build_getfamily(void *buf, size_t sz);

/* Build NL80211_CMD_GET_REG (no attributes). */
ssize_t nl80211__build_get_reg(void *buf, size_t sz, int family_id);

/* Build NL80211_CMD_REQ_SET_REG with NL80211_ATTR_REG_ALPHA2 (2 bytes,
 * no NUL — kernel copies exactly sizeof(alpha2)). */
ssize_t nl80211__build_set_reg(void *buf, size_t sz, int family_id,
                               const char *alpha2);

/* Build NL80211_CMD_GET_WIPHY with NL80211_ATTR_WIPHY=phy_idx. */
ssize_t nl80211__build_get_wiphy(void *buf, size_t sz, int family_id,
                                 int phy_idx);

/* Build NL80211_CMD_SET_WIPHY setting TX power.
 * setting: 0=AUTO, 1=LIMITED, 2=FIXED. mBm ignored for AUTO. */
ssize_t nl80211__build_set_txpower(void *buf, size_t sz, int family_id,
                                   int phy_idx, int setting, int32_t mBm);

/* Build NL80211_CMD_SET_POWER_SAVE with IFINDEX + PS_STATE. */
ssize_t nl80211__build_set_ps(void *buf, size_t sz, int family_id,
                              int ifindex, bool enabled);

/* ---- message parsing helpers ---- */

/* Parse a NLMSG_ERROR reply. Returns 0 if the kernel acked the
 * request (error==0), -1 with errno set to the kernel's error code
 * otherwise. Returns -1 with errno=EINVAL if buf is not an error. */
int nl80211__parse_ack(const void *buf, size_t sz);

/* Parse a CTRL_CMD_NEWFAMILY reply: extract CTRL_ATTR_FAMILY_ID.
 * Returns 0 on success, -1 with errno=ENOENT if not present. */
int nl80211__parse_family_id(const void *buf, size_t sz, int *out_id);

/* Parse a NL80211_CMD_NEW_WIPHY reply: extract WIPHY_TX_POWER_LEVEL.
 * Returns 0 on success, -1 with errno=ENOENT if not present. */
int nl80211__parse_txpower(const void *buf, size_t sz, int32_t *out_mBm);

/* Parse a NL80211_CMD_NEW_REG_RULE reply: extract REG_ALPHA2.
 * Returns 0 on success, -1 with errno=ENOENT if not present. */
int nl80211__parse_alpha2(const void *buf, size_t sz,
                          char *out, size_t outsz);

#endif /* ZENCTL_USB_NL80211_H */
