/* firmware.c - zenctl firmware subcommand.
 *
 * Implements:
 *   zenctl firmware dmi <field> | --all --json
 *   zenctl firmware efi {list|get|set|delete} ...
 *   zenctl firmware acpi {list|get} ...
 *   zenctl firmware bios {list|get|set} ...
 *
 * Backed by the libzenctl firmware API (DMI, efivarfs, ACPI tables,
 * firmware-attributes class). Binary payloads (EFI variable data,
 * ACPI table bytes) are emitted as hex on stdout; for EFI set the
 * payload comes from a file.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "zenctl/firmware.h"

#include "../output.h"
#include "common.h"

/* Standard EFI attribute bits (UEFI spec 8.2). */
#define EFI_VAR_NON_VOLATILE       0x00000001u
#define EFI_VAR_BOOTSERVICE_ACCESS 0x00000002u
#define EFI_VAR_RUNTIME_ACCESS     0x00000004u

/* The DMI fields exposed under /sys/class/dmi/id/. */
static const char *const DMI_FIELDS[] = {
    "bios_vendor", "bios_version", "bios_date", "bios_release",
    "ec_firmware_release", "sys_vendor",
    "product_name", "product_version", "product_sku", "product_family",
    "product_uuid", "product_serial",
    "board_vendor", "board_name", "board_version",
    "board_serial", "board_asset_tag",
    "chassis_vendor", "chassis_type", "chassis_version",
    "chassis_serial", "chassis_asset_tag",
    NULL
};

/* ── DMI ────────────────────────────────────────────────────────── */

static int fw_dmi(int argc, char **argv, bool json)
{
    bool want_all = cli_has_flag(argc, argv, "--all");
    if (!want_all) {
        if (argc < 1)
            return cli_usage(json, "zenctl firmware dmi <field> | --all");
        const char *field = argv[0];
        if (strcmp(field, "--all") == 0) want_all = true;
        else {
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            char *s = NULL;
            if (zenctl_fw_dmi_get(field, &s, &err) != 0)
                return cli_err(json, &err);
            if (json) {
                out_json_ok_begin();
                bool first = true;
                out_json_field_string(&first, field, s);
                out_json_ok_end();
            } else {
                out_kv(field, s);
            }
            free(s);
            return 0;
        }
    }

    if (json) {
        out_json_ok_begin();
        fputc('{', stdout);
        bool first = true;
        for (int i = 0; DMI_FIELDS[i]; i++) {
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            char *s = NULL;
            if (zenctl_fw_dmi_get(DMI_FIELDS[i], &s, NULL) == 0) {
                out_json_field_string(&first, DMI_FIELDS[i], s);
                free(s);
            }
        }
        fputc('}', stdout);
        out_json_ok_end();
    } else {
        for (int i = 0; DMI_FIELDS[i]; i++) {
            zenctl_err_t err;
            memset(&err, 0, sizeof(err));
            char *s = NULL;
            if (zenctl_fw_dmi_get(DMI_FIELDS[i], &s, NULL) == 0) {
                out_kv(DMI_FIELDS[i], s);
                free(s);
            }
        }
    }
    return 0;
}

/* ── EFI ────────────────────────────────────────────────────────── */

static void print_hex(const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", data[i]);
}

static int fw_efi_list(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_efi_var_t *list = NULL; int n = 0;
    if (zenctl_fw_efi_enumerate(&list, &n, &err) != 0)
        return cli_err(json, &err);

    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            out_json_field_string(&first, "name", list[i].name);
            out_json_field_string(&first, "guid", list[i].guid);
            out_json_field_int(&first, "attributes", (int64_t)list[i].attributes);
            out_json_field_int(&first, "size", (int64_t)list[i].data_size);
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"NAME", "GUID", "ATTR", "SIZE"};
        out_table_header(h, 4);
        for (int i = 0; i < n; i++) {
            char ab[16], sb[16];
            snprintf(ab, sizeof(ab), "0x%x", list[i].attributes);
            snprintf(sb, sizeof(sb), "%zu", list[i].data_size);
            const char *r[] = {list[i].name, list[i].guid, ab, sb};
            out_table_row(r, 4);
        }
    }
    free(list);
    return 0;
}

static int fw_efi_get(int argc, char **argv, bool json)
{
    if (argc < 2)
        return cli_usage(json, "zenctl firmware efi get <name> <guid>");
    const char *name = argv[0];
    const char *guid = argv[1];

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    uint8_t *data = NULL; size_t sz = 0;
    if (zenctl_fw_efi_get(name, guid, &data, &sz, &err) != 0)
        return cli_err(json, &err);

    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "name", name);
        out_json_field_string(&first, "guid", guid);
        out_json_field_int(&first, "size", (int64_t)sz);
        fputs(",\"data\":\"", stdout);
        for (size_t i = 0; i < sz; i++) printf("%02x", data[i]);
        fputs("\"", stdout);
        out_json_ok_end();
    } else {
        out_kv("name", name);
        out_kv("guid", guid);
        out_kv_int("size", (int64_t)sz);
        fputs("data: ", stdout);
        print_hex(data, sz);
        putchar('\n');
    }
    free(data);
    return 0;
}

static int read_payload_file(const char *path, uint8_t **out, size_t *out_sz,
                             zenctl_err_t *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        cli_make_err(err, ZENCTL_ERR_EIO, strerror(errno), path);
        return -1;
    }
    size_t cap = 4096, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { close(fd); cli_make_err(err, ZENCTL_ERR_NOMEM, "oom", path); return -1; }
    for (;;) {
        if (len == cap) {
            size_t nc = cap * 2; uint8_t *nb = realloc(buf, nc);
            if (!nb) { free(buf); close(fd); cli_make_err(err, ZENCTL_ERR_NOMEM, "oom", path); return -1; }
            buf = nb; cap = nc;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno; free(buf); close(fd);
            cli_make_err(err, ZENCTL_ERR_EIO, strerror(e), path);
            return -1;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    *out = buf; *out_sz = len;
    return 0;
}

static int fw_efi_set(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 3)
        return cli_usage(json, "zenctl firmware efi set <name> <guid> <file>");
    const char *name = argv[0];
    const char *guid = argv[1];
    const char *file = argv[2];

    if (cli_require_root(json)) return -1;

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    uint8_t *data = NULL; size_t sz = 0;
    if (read_payload_file(file, &data, &sz, &err) != 0)
        return cli_err(json, &err);

    uint32_t attr = EFI_VAR_NON_VOLATILE | EFI_VAR_BOOTSERVICE_ACCESS |
                    EFI_VAR_RUNTIME_ACCESS;

    if (dry_run) {
        char b[160];
        snprintf(b, sizeof(b), "set efi %s-%s <= %s (%zu bytes)",
                 name, guid, file, sz);
        cli_dryrun(json, b);
        free(data);
        return 0;
    }

    int rc = 0;
    if (zenctl_fw_efi_set(name, guid, attr, data, sz, &err) == 0) {
        if (json) {
            out_json_ok_begin();
            bool first = true;
            out_json_field_string(&first, "name", name);
            out_json_field_string(&first, "guid", guid);
            out_json_field_int(&first, "size", (int64_t)sz);
            out_json_field_int(&first, "attributes", (int64_t)attr);
            out_json_ok_end();
        } else {
            out_kv("name", name);
            out_kv("guid", guid);
            out_kv_int("wrote_bytes", (int64_t)sz);
        }
    } else rc = cli_err(json, &err);
    free(data);
    return rc;
}

static int fw_efi_delete(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 2)
        return cli_usage(json, "zenctl firmware efi delete <name> <guid>");
    const char *name = argv[0];
    const char *guid = argv[1];

    if (cli_require_root(json)) return -1;
    if (!confirm) {
        if (json) out_json_error(ZENCTL_ERR_EPERM,
                       "deleting an EFI variable requires --confirm");
        else out_err_code(ZENCTL_ERR_EPERM,
                       "deleting an EFI variable requires --confirm");
        return -1;
    }
    if (dry_run) {
        char b[160];
        snprintf(b, sizeof(b), "delete efi %s-%s", name, guid);
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    if (zenctl_fw_efi_delete(name, guid, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "deleted", name);
        out_json_field_string(&first, "guid", guid);
        out_json_ok_end();
    } else {
        out_kv("deleted", name);
        out_kv("guid", guid);
    }
    return 0;
}

static int fw_efi(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl firmware efi <list|get|set|delete> ...");
    const char *sub = argv[0];
    if (strcmp(sub, "list")   == 0) return fw_efi_list(json);
    if (strcmp(sub, "get")    == 0) return fw_efi_get(argc - 1, argv + 1, json);
    if (strcmp(sub, "set")    == 0) return fw_efi_set(argc - 1, argv + 1, json, dry_run);
    if (strcmp(sub, "delete") == 0) return fw_efi_delete(argc - 1, argv + 1, json, dry_run, confirm);
    return cli_usage(json, "zenctl firmware efi <list|get|set|delete> ...");
}

/* ── ACPI ───────────────────────────────────────────────────────── */

static int fw_acpi_list(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char **list = NULL; int n = 0;
    if (zenctl_fw_acpi_list_tables(&list, &n, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < n; i++) {
            if (i) fputc(',', stdout);
            out_json_escape(list[i]);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        for (int i = 0; i < n; i++) puts(list[i]);
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    return 0;
}

static int fw_acpi_get(int argc, char **argv, bool json)
{
    if (argc < 1)
        return cli_usage(json, "zenctl firmware acpi get <name>");
    const char *name = argv[0];
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    uint8_t *data = NULL; size_t sz = 0;
    if (zenctl_fw_acpi_get_table(name, &data, &sz, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "name", name);
        out_json_field_int(&first, "size", (int64_t)sz);
        fputs(",\"data\":\"", stdout);
        for (size_t i = 0; i < sz; i++) printf("%02x", data[i]);
        fputs("\"", stdout);
        out_json_ok_end();
    } else {
        out_kv("name", name);
        out_kv_int("size", (int64_t)sz);
        fputs("data: ", stdout);
        print_hex(data, sz);
        putchar('\n');
    }
    free(data);
    return 0;
}

static int fw_acpi(int argc, char **argv, bool json)
{
    if (argc < 1) return cli_usage(json, "zenctl firmware acpi <list|get> ...");
    if (strcmp(argv[0], "list") == 0) return fw_acpi_list(json);
    if (strcmp(argv[0], "get")  == 0) return fw_acpi_get(argc - 1, argv + 1, json);
    return cli_usage(json, "zenctl firmware acpi <list|get> ...");
}

/* ── BIOS settings ──────────────────────────────────────────────── */

static void bios_emit(bool json, bool *first, const zenctl_bios_setting_t *s)
{
    if (json) {
        out_json_field_string(first, "name", s->name);
        if (s->type[0])           out_json_field_string(first, "type", s->type);
        if (s->current_value[0])  out_json_field_string(first, "current_value", s->current_value);
        if (s->default_value[0])  out_json_field_string(first, "default_value", s->default_value);
        if (s->display_name[0])   out_json_field_string(first, "display_name", s->display_name);
    } else {
        out_kv("name", s->name);
        if (s->type[0])          out_kv("type", s->type);
        if (s->current_value[0]) out_kv("current_value", s->current_value);
        if (s->default_value[0]) out_kv("default_value", s->default_value);
        if (s->display_name[0])  out_kv("display_name", s->display_name);
    }
}

static int fw_bios_list(bool json)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int count = 0;
    if (zenctl_fw_bios_setting_count(&count, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        fputc('[', stdout);
        for (int i = 0; i < count; i++) {
            zenctl_bios_setting_t s;
            memset(&s, 0, sizeof(s));
            if (zenctl_fw_bios_get_setting(i, &s, NULL) != 0) continue;
            if (i) fputc(',', stdout);
            fputc('{', stdout);
            bool first = true;
            bios_emit(json, &first, &s);
            fputc('}', stdout);
        }
        fputc(']', stdout);
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = {"NAME", "TYPE", "CURRENT", "DEFAULT"};
        out_table_header(h, 4);
        for (int i = 0; i < count; i++) {
            zenctl_bios_setting_t s;
            memset(&s, 0, sizeof(s));
            if (zenctl_fw_bios_get_setting(i, &s, NULL) != 0) continue;
            const char *r[] = {s.name,
                               s.type[0] ? s.type : "-",
                               s.current_value[0] ? s.current_value : "-",
                               s.default_value[0] ? s.default_value : "-"};
            out_table_row(r, 4);
        }
    }
    return 0;
}

static int fw_bios_get(int argc, char **argv, bool json)
{
    if (argc < 1)
        return cli_usage(json, "zenctl firmware bios get <setting>");
    const char *want = argv[0];
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int count = 0;
    if (zenctl_fw_bios_setting_count(&count, &err) != 0)
        return cli_err(json, &err);
    for (int i = 0; i < count; i++) {
        zenctl_bios_setting_t s;
        memset(&s, 0, sizeof(s));
        if (zenctl_fw_bios_get_setting(i, &s, NULL) != 0) continue;
        if (strcmp(s.name, want) != 0) continue;
        if (json) {
            out_json_ok_begin();
            fputc('{', stdout);
            bool first = true;
            bios_emit(json, &first, &s);
            fputc('}', stdout);
            out_json_ok_end();
        } else {
            bios_emit(json, NULL, &s);
        }
        return 0;
    }
    cli_make_err(&err, ZENCTL_ERR_ENOENT,
                 "BIOS setting not found", want);
    return cli_err(json, &err);
}

static int fw_bios_set(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 2)
        return cli_usage(json,
            "zenctl firmware bios set <setting> <value> [--password <pw>]");
    const char *name = argv[0];
    const char *val  = argv[1];
    const char *pw   = cli_opt(argc, argv, "--password");

    if (cli_require_root(json)) return -1;
    if (dry_run) {
        char b[160];
        snprintf(b, sizeof(b), "set bios %s=%s%s", name, val,
                 pw ? " (with password)" : "");
        cli_dryrun(json, b);
        return 0;
    }
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    if (zenctl_fw_bios_set_setting(name, val, pw, &err) != 0)
        return cli_err(json, &err);
    if (json) {
        out_json_ok_begin();
        bool first = true;
        out_json_field_string(&first, "name", name);
        out_json_field_string(&first, "value", val);
        out_json_ok_end();
    } else {
        out_kv("name", name);
        out_kv("set value", val);
    }
    return 0;
}

static int fw_bios(int argc, char **argv, bool json, bool dry_run)
{
    if (argc < 1) return cli_usage(json, "zenctl firmware bios <list|get|set> ...");
    if (strcmp(argv[0], "list") == 0) return fw_bios_list(json);
    if (strcmp(argv[0], "get")  == 0) return fw_bios_get(argc - 1, argv + 1, json);
    if (strcmp(argv[0], "set")  == 0) return fw_bios_set(argc - 1, argv + 1, json, dry_run);
    return cli_usage(json, "zenctl firmware bios <list|get|set> ...");
}

/* ── entry ──────────────────────────────────────────────────────── */

int cmd_firmware(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    if (argc < 1)
        return cli_usage(json, "zenctl firmware <dmi|efi|acpi|bios> ...");
    const char *sub = argv[0];
    if (strcmp(sub, "dmi")   == 0) return fw_dmi(argc - 1, argv + 1, json);
    if (strcmp(sub, "efi")   == 0) return fw_efi(argc - 1, argv + 1, json, dry_run, confirm);
    if (strcmp(sub, "acpi")  == 0) return fw_acpi(argc - 1, argv + 1, json);
    if (strcmp(sub, "bios")  == 0) return fw_bios(argc - 1, argv + 1, json, dry_run);
    return cli_usage(json, "zenctl firmware <dmi|efi|acpi|bios> ...");
}
