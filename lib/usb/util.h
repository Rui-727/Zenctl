/* util.h - private filesystem helpers for the USB / BT / wireless modules.
 *
 * Tiny wrappers around opendir/readdir and path joining. Not part of
 * the public API; only linked into the USB/BT/wireless translation
 * units.
 */
#ifndef ZENCTL_USB_UTIL_H
#define ZENCTL_USB_UTIL_H

#include <stddef.h>

#include "zenctl/zenctl.h"

/* Heap-allocated "<dir>/<name>". Caller frees. Returns NULL on OOM. */
char *zenctl_util_path_join(const char *dir, const char *name);

/* NULL-terminated array of heap-allocated entry names. Caller frees
 * each entry with free() and the array with free(). Skips "." and
 * "..". Returns NULL on failure with *err set. */
char **zenctl_util_list_dir(const char *dir, zenctl_err_t *err);

#endif /* ZENCTL_USB_UTIL_H */
