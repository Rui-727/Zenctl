/* rfkill.h - private rfkill helpers for the USB / BT / wireless modules.
 *
 * rfkill is shared by the Bluetooth and wireless domains. These
 * helpers locate the rfkill device associated with a given adapter or
 * PHY and read / write its soft-block state. Not part of the public
 * API.
 */
#ifndef ZENCTL_USB_RFKILL_H
#define ZENCTL_USB_RFKILL_H

#include <stdbool.h>

#include "zenctl/zenctl.h"

/* Find an rfkill index whose "type" file matches want_type and whose
 * "name" file matches want_name. If want_name is NULL, the first
 * rfkill of the requested type is returned. Returns the index >= 0 on
 * success, or -1 on failure with *err set. */
int zenctl_rfkill_find(const char *want_name, const char *want_type, zenctl_err_t *err);

/* Read the soft-block state of rfkill index N. */
int zenctl_rfkill_get_soft(int idx, bool *out, zenctl_err_t *err);

/* Write the soft-block state of rfkill index N. */
int zenctl_rfkill_set_soft(int idx, bool blocked, zenctl_err_t *err);

#endif /* ZENCTL_USB_RFKILL_H */
