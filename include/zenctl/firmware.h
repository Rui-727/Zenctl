/* firmware.h - firmware / BIOS domain API
 *
 * Wraps DMI/SMBIOS (/sys/class/dmi/id/), efivarfs
 * (/sys/firmware/efi/efivars/), ACPI tables
 * (/sys/firmware/acpi/tables/), and the firmware-attributes class
 * (/sys/class/firmware-attributes/<vendor>/attributes/). See
 * docs/KERNEL_USB_BT_FW.md section 4.
 */
#ifndef ZENCTL_FIRMWARE_H
#define ZENCTL_FIRMWARE_H

#include "zenctl.h"

/* DMI / SMBIOS info. field is one of: bios_vendor, bios_version,
 * bios_date, bios_release, ec_firmware_release, sys_vendor,
 * product_name, product_version, product_sku, product_family,
 * board_vendor, board_name, board_version, board_asset_tag,
 * chassis_vendor, chassis_type, chassis_version, chassis_asset_tag.
 *
 * The root-only fields product_uuid, product_serial, board_serial,
 * chassis_serial require CAP_SYS_ADMIN (or root). On failure returns
 * -1; *err->code is ZENCTL_ERR_ENOENT if the field is not present on
 * this platform, ZENCTL_ERR_EPERM if reading is denied. The returned
 * string is heap-allocated; caller frees. */
int zenctl_fw_dmi_get(const char *field, char **out, zenctl_err_t *err);

/* EFI variables (efivarfs at /sys/firmware/efi/efivars/). */
typedef struct {
    char     name[256];   /* variable name without the GUID suffix */
    char     guid[37];    /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
    uint32_t attributes;  /* EFI_VARIABLE_* bitmask */
    size_t   data_size;   /* payload size in bytes (excludes the 4-byte attr prefix) */
} zenctl_efi_var_t;

/* Enumerate every variable in /sys/firmware/efi/efivars/. Returns a
 * heap-allocated array; caller frees with free(). The name and guid
 * fields of each entry are filled; attributes and data_size are
 * populated by reading the file header. On systems where efivarfs is
 * not mounted returns ZENCTL_ERR_ENOTSUP. */
int zenctl_fw_efi_enumerate(zenctl_efi_var_t **out_list, int *out_count, zenctl_err_t *err);

/* Read an EFI variable. *out_data is heap-allocated; caller frees. */
int zenctl_fw_efi_get(const char *name, const char *guid,
                      uint8_t **out_data, size_t *out_size, zenctl_err_t *err);

/* Write or create an EFI variable. The attributes prefix is prepended
 * automatically. Requires root. If the variable file exists and is
 * marked immutable (FS_IMMUTABLE_FL), the immutable flag is cleared
 * for the duration of the write and re-applied afterwards; this
 * requires CAP_LINUX_IMMUTABLE. */
int zenctl_fw_efi_set(const char *name, const char *guid, uint32_t attributes,
                      const uint8_t *data, size_t size, zenctl_err_t *err);

/* Delete an EFI variable. Implemented by writing 4 bytes of zero
 * attributes and zero data. Requires root. Clears the immutable flag
 * if set. */
int zenctl_fw_efi_delete(const char *name, const char *guid, zenctl_err_t *err);

/* ACPI tables. Names are the 4-character ACPI signatures (DSDT, SSDT,
 * FACP, APIC, MCFG, HPET, ...). list_tables returns heap-allocated
 * strings; caller frees each and the array. get_table returns the
 * raw table bytes; caller frees. */
int zenctl_fw_acpi_list_tables(char ***out_list, int *out_count, zenctl_err_t *err);
int zenctl_fw_acpi_get_table(const char *name, uint8_t **out_data, size_t *out_size, zenctl_err_t *err);

/* WMI / BIOS settings via the firmware-attributes class. */
typedef struct {
    char name[128];          /* attribute directory name */
    char type[32];           /* "string", "integer", "enumeration", "password", "ordered-list" */
    char current_value[256];
    char default_value[256];
    char display_name[256];
} zenctl_bios_setting_t;

/* Returns the number of settings under
 * /sys/class/firmware-attributes/<vendor>/attributes/, or -1 on error
 * (no firmware-attributes class registered). */
int zenctl_fw_bios_setting_count(int *out, zenctl_err_t *err);

/* Fetch one setting by index (0-based). */
int zenctl_fw_bios_get_setting(int index, zenctl_bios_setting_t *out, zenctl_err_t *err);

/* Write a new value. If password is non-NULL and non-empty, it is
 * first written to
 * /sys/class/firmware-attributes/<vendor>/authentication/<role>/current_password
 * (best-effort: the role name is "bios-admin" by default; callers
 * needing a different role should write the password file directly).
 * Then <value> is written to
 * attributes/<name>/current_value. */
int zenctl_fw_bios_set_setting(const char *name, const char *value,
                               const char *password, zenctl_err_t *err);

#endif /* ZENCTL_FIRMWARE_H */
