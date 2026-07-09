/* cmd.h - dispatch table for zenctl domain handlers
 *
 * Each handler receives (argc, argv) where argv[0] is the subcommand
 * (e.g. "get" or "set"), argv[1] is the target (e.g. "governor"), and
 * argv[2..] are positional/value arguments. Global flags --json,
 * --dry-run, --confirm have already been extracted by main.c and are
 * passed as boolean parameters.
 *
 * Returns 0 on success, 1 on error.
 */
#ifndef ZENCTL_CLI_CMD_H
#define ZENCTL_CLI_CMD_H

#include <stdbool.h>

typedef int (*cmd_handler_fn)(int argc, char **argv,
                              bool json, bool dry_run, bool confirm);

/* Implemented domains */
int cmd_cpu(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_mem(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_storage(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_net(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_pcie(int argc, char **argv, bool json, bool dry_run, bool confirm);

/* Stubs - print "not yet implemented" and return 1 */
int cmd_gpu(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_thermal(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_power(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_usb(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_bt(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_wireless(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_firmware(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_profile(int argc, char **argv, bool json, bool dry_run, bool confirm);
int cmd_caps(int argc, char **argv, bool json, bool dry_run, bool confirm);

#endif /* ZENCTL_CLI_CMD_H */
