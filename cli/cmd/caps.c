/* cmd_caps.c - capability matrix
 *
 * Lists the supported domains and their capability status. For now
 * this is a static table; eventually it queries the library's
 * capability API (zenctl_query_cap) for each known key.
 *
 * Usage:
 *   zenctl caps              # list all domains
 *   zenctl caps <domain>     # list keys for one domain
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "output.h"
#include "cmd_util.h"

struct cap_entry {
    const char *domain;
    const char *status;   /* "available", "readonly", "unavailable" */
};

static const struct cap_entry cap_table[] = {
    { "cpu",      "available" },
    { "mem",      "available" },
    { "storage",  "available" },
    { "net",      "available" },
    { "pcie",     "available" },
    { "gpu",      "stub" },
    { "thermal",  "stub" },
    { "power",    "stub" },
    { "usb",      "stub" },
    { "bt",       "stub" },
    { "wireless", "stub" },
    { "firmware", "stub" },
    { "profile",  "stub" },
    { NULL, NULL }
};

int cmd_caps(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)dry_run; (void)confirm;
    const char *filter = cmd_positional(argc, argv, 0);

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_array_begin(&f, "caps");
        bool arr_first = true;
        for (const struct cap_entry *e = cap_table; e->domain; e++) {
            if (filter && strcmp(filter, e->domain) != 0)
                continue;
            out_json_separator(&arr_first);
            out_json_open_object();
            bool of = true;
            out_json_field_string(&of, "domain", e->domain);
            out_json_field_string(&of, "status", e->status);
            out_json_close_object();
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "DOMAIN", "STATUS" };
        out_table_header(h, 2);
        for (const struct cap_entry *e = cap_table; e->domain; e++) {
            if (filter && strcmp(filter, e->domain) != 0)
                continue;
            const char *row[] = { e->domain, e->status };
            out_table_row(row, 2);
        }
    }
    return 0;
}
