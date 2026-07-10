/* wireless.h - Wireless (Wi-Fi / cfg80211 / nl80211) domain API
 *
 * Wraps the wireless PHY sysfs (/sys/class/ieee80211/phy<N>/), the
 * rfkill interface, and the nl80211 genetlink interface. See
 * docs/KERNEL_USB_BT_FW.md section 3.
 *
 * Regdomain: read via NL80211_CMD_GET_REG (returns the system-global
 * alpha2); set via NL80211_CMD_REQ_SET_REG (requires CAP_NET_ADMIN).
 * The regdomain is system-global; per-phy self-managed regulators are
 * not exposed by this API.
 *
 * TX power: read via NL80211_CMD_GET_WIPHY (returns mBm = 100*dBm);
 * set via NL80211_CMD_SET_WIPHY with NL80211_TX_POWER_FIXED (requires
 * CAP_NET_ADMIN). Pass mBm == -1 to set automatic TX power control.
 *
 * Power save: set via NL80211_CMD_SET_POWER_SAVE (requires
 * CAP_NET_ADMIN). nl80211 has no GET_POWER_SAVE; the getter returns
 * ZENCTL_ERR_ENOTSUP (the current state can be read with `iw dev
 * <iface> info` or via debugfs, both of which are fragile and out of
 * scope for the library API).
 */
#ifndef ZENCTL_WIRELESS_H
#define ZENCTL_WIRELESS_H

#include "zenctl.h"

typedef struct zenctl_wireless zenctl_wireless_t;

/* Open by phy name, e.g. "phy0". The name is the directory under
 * /sys/class/ieee80211/. Returns NULL and sets *err on failure. */
zenctl_wireless_t *zenctl_wireless_open(const char *phy, zenctl_err_t *err);
void               zenctl_wireless_close(zenctl_wireless_t *wl);

/* Regulatory domain. get returns an ISO 3166-1 alpha-2 string (e.g.
 * "US", "DE") via NL80211_CMD_GET_REG. set issues
 * NL80211_CMD_REQ_SET_REG; requires CAP_NET_ADMIN. The regdomain is
 * system-global — the phy handle is taken for API symmetry but the
 * change applies to every phy. Returns 0 / -1. */
int zenctl_wireless_get_regdomain(zenctl_wireless_t *wl, char **out, zenctl_err_t *err);
int zenctl_wireless_set_regdomain(zenctl_wireless_t *wl, const char *country, zenctl_err_t *err);

/* TX power in mBm (100 * dBm). get reads NL80211_ATTR_WIPHY_TX_POWER_LEVEL
 * from NL80211_CMD_GET_WIPHY (returns 0 mBm as a valid value). set
 * issues NL80211_CMD_SET_WIPHY with NL80211_TX_POWER_FIXED and the
 * given level; requires CAP_NET_ADMIN. Pass mBm == -1 to set
 * NL80211_TX_POWER_AUTOMATIC (driver picks). Returns 0 / -1. */
int zenctl_wireless_get_txpower(zenctl_wireless_t *wl, int32_t *out_mBm, zenctl_err_t *err);
int zenctl_wireless_set_txpower(zenctl_wireless_t *wl, int32_t mBm, zenctl_err_t *err);

/* 802.11 power save. set issues NL80211_CMD_SET_POWER_SAVE on the
 * first interface bound to this phy; requires CAP_NET_ADMIN. get
 * returns ZENCTL_ERR_ENOTSUP — nl80211 has no GET_POWER_SAVE. */
int zenctl_wireless_get_power_save(zenctl_wireless_t *wl, bool *out, zenctl_err_t *err);
int zenctl_wireless_set_power_save(zenctl_wireless_t *wl, bool on, zenctl_err_t *err);

/* rfkill soft-block. The PHY's rfkill device is located by name match
 * against /sys/class/rfkill/rfkill<N>/name (which is usually the phy
 * name, "phy0"). */
int zenctl_wireless_get_rfkill_blocked(zenctl_wireless_t *wl, bool *out, zenctl_err_t *err);
int zenctl_wireless_set_rfkill_blocked(zenctl_wireless_t *wl, bool blocked, zenctl_err_t *err);

/* Enumerate phy names (phy0, phy1, ...) under /sys/class/ieee80211/.
 * Heap-allocated strings; caller frees each and the array. */
int zenctl_wireless_enumerate(char ***out_list, int *out_count, zenctl_err_t *err);

#endif /* ZENCTL_WIRELESS_H */
