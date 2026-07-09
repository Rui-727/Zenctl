/* cpu.c - CPU domain implementation
 *
 * Implements the API declared in include/zenctl/cpu.h against the
 * sysfs surface documented in docs/KERNEL_CPU_MEM.md.
 *
 * Frequency convention: the public API uses Hz (int64_t). The kernel
 * uses kHz for every cpufreq file. Hz <-> kHz conversion happens at
 * this boundary, never inside the helpers and never in the caller.
 *
 * String writes are sent verbatim with no trailing newline (the
 * kernel's sysfs parsers tolerate but do not require newlines, and
 * some reject leading/trailing whitespace). Boolean writes always
 * send the canonical "0" or "1".
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include "zenctl/internal.h"

struct zenctl_cpu {
    int index;
};

/* ── Open / close ────────────────────────────────────────────────── */

zenctl_cpu_t *zenctl_cpu_open(int cpu_index, zenctl_err_t *err)
{
    if (cpu_index < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "cpu index must be >= 0", "zenctl_cpu_open");
        return NULL;
    }

    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d", cpu_index);
    if (access(path, F_OK) != 0) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        "CPU directory does not exist", path);
        return NULL;
    }

    zenctl_cpu_t *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM,
                        "calloc failed", "zenctl_cpu_open");
        return NULL;
    }
    cpu->index = cpu_index;
    return cpu;
}

void zenctl_cpu_close(zenctl_cpu_t *cpu)
{
    free(cpu);
}

/* ── Governor ────────────────────────────────────────────────────── */
/*
 * The returned string is malloc'd. The caller frees it with free().
 */

static void cpu_path(zenctl_cpu_t *cpu, char *buf, size_t bufsz,
                     const char *suffix)
{
    snprintf(buf, bufsz,
             "/sys/devices/system/cpu/cpu%d/%s", cpu->index, suffix);
}

int zenctl_cpu_get_governor(zenctl_cpu_t *cpu, char **out,
                            zenctl_err_t *err)
{
    if (!cpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu or out", "zenctl_cpu_get_governor");
        return -1;
    }
    char path[128], buf[256];
    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_governor");
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return -1;

    char *s = strdup(buf);
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM,
                        "strdup failed", "zenctl_cpu_get_governor");
        return -1;
    }
    *out = s;
    return 0;
}

int zenctl_cpu_set_governor(zenctl_cpu_t *cpu, const char *gov,
                            zenctl_err_t *err)
{
    if (!cpu || !gov) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu or gov", "zenctl_cpu_set_governor");
        return -1;
    }
    if (*gov == '\0') {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "governor name is empty", "zenctl_cpu_set_governor");
        return -1;
    }

    /* Validate against scaling_available_governors. The kernel does a
     * case-insensitive match, so we do too. */
    char avail_path[128], avail[1024];
    cpu_path(cpu, avail_path, sizeof(avail_path),
             "cpufreq/scaling_available_governors");
    if (zenctl__read_file_string(avail_path, avail, sizeof(avail), err) != 0)
        return -1;

    int matched = 0;
    char *save = NULL;
    for (char *tok = strtok_r(avail, " \t\n", &save);
         tok != NULL;
         tok = strtok_r(NULL, " \t\n", &save)) {
        if (strcasecmp(tok, gov) == 0) { matched = 1; break; }
    }
    if (!matched) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "governor not in scaling_available_governors",
                        gov);
        return -1;
    }

    char path[128];
    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_governor");
    return zenctl__write_file_string(path, gov, err);
}

/* ── Frequency (Hz at the API, kHz at the kernel) ────────────────── */

static int read_cpuinfo_bound(zenctl_cpu_t *cpu, const char *attr,
                              int64_t *out_khz, zenctl_err_t *err)
{
    char path[128];
    cpu_path(cpu, path, sizeof(path), attr);
    return zenctl__read_file_i64(path, out_khz, err);
}

int zenctl_cpu_get_freq_min(zenctl_cpu_t *cpu, int64_t *out,
                            zenctl_err_t *err)
{
    if (!cpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu or out", "zenctl_cpu_get_freq_min");
        return -1;
    }
    char path[128];
    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_min_freq");
    int64_t khz;
    if (zenctl__read_file_i64(path, &khz, err) != 0) return -1;
    *out = khz * 1000;
    return 0;
}

int zenctl_cpu_get_freq_max(zenctl_cpu_t *cpu, int64_t *out,
                            zenctl_err_t *err)
{
    if (!cpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu or out", "zenctl_cpu_get_freq_max");
        return -1;
    }
    char path[128];
    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_max_freq");
    int64_t khz;
    if (zenctl__read_file_i64(path, &khz, err) != 0) return -1;
    *out = khz * 1000;
    return 0;
}

int zenctl_cpu_set_freq_min(zenctl_cpu_t *cpu, int64_t hz,
                            zenctl_err_t *err)
{
    if (!cpu) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu", "zenctl_cpu_set_freq_min");
        return -1;
    }
    if (hz < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "frequency must be >= 0", "zenctl_cpu_set_freq_min");
        return -1;
    }

    int64_t khz = hz / 1000;

    /* Range-check against cpuinfo_min_freq / cpuinfo_max_freq. */
    int64_t hw_min = 0, hw_max = 0;
    if (read_cpuinfo_bound(cpu, "cpufreq/cpuinfo_min_freq", &hw_min, err) != 0)
        return -1;
    if (read_cpuinfo_bound(cpu, "cpufreq/cpuinfo_max_freq", &hw_max, err) != 0)
        return -1;
    if (khz < hw_min) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "below cpuinfo_min_freq", "zenctl_cpu_set_freq_min");
        return -1;
    }
    if (khz > hw_max) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "above cpuinfo_max_freq", "zenctl_cpu_set_freq_min");
        return -1;
    }

    /* Enforce scaling_min_freq <= scaling_max_freq to avoid EINVAL. */
    char path[128];
    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_max_freq");
    int64_t cur_max = 0;
    if (zenctl__read_file_i64(path, &cur_max, err) != 0) return -1;
    if (khz > cur_max) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "above current scaling_max_freq",
                        "zenctl_cpu_set_freq_min");
        return -1;
    }

    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_min_freq");
    return zenctl__write_file_i64(path, khz, err);
}

int zenctl_cpu_set_freq_max(zenctl_cpu_t *cpu, int64_t hz,
                            zenctl_err_t *err)
{
    if (!cpu) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu", "zenctl_cpu_set_freq_max");
        return -1;
    }
    if (hz < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "frequency must be >= 0", "zenctl_cpu_set_freq_max");
        return -1;
    }

    int64_t khz = hz / 1000;

    int64_t hw_min = 0, hw_max = 0;
    if (read_cpuinfo_bound(cpu, "cpufreq/cpuinfo_min_freq", &hw_min, err) != 0)
        return -1;
    if (read_cpuinfo_bound(cpu, "cpufreq/cpuinfo_max_freq", &hw_max, err) != 0)
        return -1;
    if (khz < hw_min) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "below cpuinfo_min_freq", "zenctl_cpu_set_freq_max");
        return -1;
    }
    if (khz > hw_max) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "above cpuinfo_max_freq", "zenctl_cpu_set_freq_max");
        return -1;
    }

    char path[128];
    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_min_freq");
    int64_t cur_min = 0;
    if (zenctl__read_file_i64(path, &cur_min, err) != 0) return -1;
    if (khz < cur_min) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "below current scaling_min_freq",
                        "zenctl_cpu_set_freq_max");
        return -1;
    }

    cpu_path(cpu, path, sizeof(path), "cpufreq/scaling_max_freq");
    return zenctl__write_file_i64(path, khz, err);
}

/* ── Core on/off ─────────────────────────────────────────────────── */
/*
 * The `online` file is only present on hotpluggable CPUs. CPU 0 on x86
 * has no such file. Missing file -> ZENCTL_ERR_ENOENT.
 *
 * This operation is destructive (offlining a core kills bound tasks).
 * The CLI layer must gate it; the library function just writes.
 */

int zenctl_cpu_get_online(zenctl_cpu_t *cpu, bool *out, zenctl_err_t *err)
{
    if (!cpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu or out", "zenctl_cpu_get_online");
        return -1;
    }
    char path[128], buf[16];
    cpu_path(cpu, path, sizeof(path), "online");
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return -1;
    *out = (buf[0] == '1');
    return 0;
}

int zenctl_cpu_set_online(zenctl_cpu_t *cpu, bool on, zenctl_err_t *err)
{
    if (!cpu) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu", "zenctl_cpu_set_online");
        return -1;
    }
    char path[128];
    cpu_path(cpu, path, sizeof(path), "online");
    return zenctl__write_file_string(path, on ? "1" : "0", err);
}

/* ── SMT ─────────────────────────────────────────────────────────── */
/*
 * SMT is a global control: /sys/devices/system/cpu/smt/control and
 * /sys/devices/system/cpu/smt/active. The cpu handle is only used so
 * the API stays uniform with the per-CPU operations.
 */

static const char *const SMT_ACTIVE_PATH =
    "/sys/devices/system/cpu/smt/active";
static const char *const SMT_CONTROL_PATH =
    "/sys/devices/system/cpu/smt/control";

int zenctl_cpu_get_smt_active(zenctl_cpu_t *cpu, bool *out,
                              zenctl_err_t *err)
{
    (void)cpu;
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_cpu_get_smt_active");
        return -1;
    }
    char buf[16];
    if (zenctl__read_file_string(SMT_ACTIVE_PATH, buf, sizeof(buf), err) != 0)
        return -1;
    *out = (buf[0] == '1');
    return 0;
}

int zenctl_cpu_set_smt_active(zenctl_cpu_t *cpu, bool on,
                              zenctl_err_t *err)
{
    (void)cpu;
    /* `forceoff` is one-way and must be gated by the CLI; we only
     * ever send "on" or "off" from this function. */
    return zenctl__write_file_string(SMT_CONTROL_PATH, on ? "on" : "off", err);
}

/* ── C-states ────────────────────────────────────────────────────── */
/*
 * /sys/devices/system/cpu/cpuN/cpuidle/stateM/disable
 * Reads "0" (enabled) or "1" (disabled). Writing requires CAP_SYS_ADMIN.
 * Disabling a state on one CPU does not affect other CPUs.
 */

static int cstate_path(zenctl_cpu_t *cpu, int state,
                       char *buf, size_t bufsz)
{
    return snprintf(buf, bufsz,
                    "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable",
                    cpu->index, state);
}

int zenctl_cpu_get_cstate_disable(zenctl_cpu_t *cpu, int state,
                                  bool *out, zenctl_err_t *err)
{
    if (!cpu || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu or out", "zenctl_cpu_get_cstate_disable");
        return -1;
    }
    if (state < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "state index must be >= 0",
                        "zenctl_cpu_get_cstate_disable");
        return -1;
    }
    char path[160], buf[16];
    cstate_path(cpu, state, path, sizeof(path));
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return -1;
    *out = (buf[0] == '1');
    return 0;
}

int zenctl_cpu_set_cstate_disable(zenctl_cpu_t *cpu, int state,
                                  bool disable, zenctl_err_t *err)
{
    if (!cpu) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL cpu", "zenctl_cpu_set_cstate_disable");
        return -1;
    }
    if (state < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "state index must be >= 0",
                        "zenctl_cpu_set_cstate_disable");
        return -1;
    }
    char path[160];
    cstate_path(cpu, state, path, sizeof(path));
    return zenctl__write_file_string(path, disable ? "1" : "0", err);
}
