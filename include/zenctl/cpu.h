/* cpu.h - CPU domain API */
#ifndef ZENCTL_CPU_H
#define ZENCTL_CPU_H

#include "zenctl.h"

typedef struct zenctl_cpu zenctl_cpu_t;

zenctl_cpu_t *zenctl_cpu_open(int cpu_index, zenctl_err_t *err);
void          zenctl_cpu_close(zenctl_cpu_t *cpu);

/* Governor */
int zenctl_cpu_get_governor(zenctl_cpu_t *cpu, char **out, zenctl_err_t *err);
int zenctl_cpu_set_governor(zenctl_cpu_t *cpu, const char *gov, zenctl_err_t *err);

/* Frequency in Hz */
int zenctl_cpu_get_freq_min(zenctl_cpu_t *cpu, int64_t *out, zenctl_err_t *err);
int zenctl_cpu_get_freq_max(zenctl_cpu_t *cpu, int64_t *out, zenctl_err_t *err);
int zenctl_cpu_set_freq_min(zenctl_cpu_t *cpu, int64_t hz, zenctl_err_t *err);
int zenctl_cpu_set_freq_max(zenctl_cpu_t *cpu, int64_t hz, zenctl_err_t *err);

/* Core on/off */
int zenctl_cpu_get_online(zenctl_cpu_t *cpu, bool *out, zenctl_err_t *err);
int zenctl_cpu_set_online(zenctl_cpu_t *cpu, bool on, zenctl_err_t *err);

/* SMT (Simultaneous Multithreading) */
int zenctl_cpu_get_smt_active(zenctl_cpu_t *cpu, bool *out, zenctl_err_t *err);
int zenctl_cpu_set_smt_active(zenctl_cpu_t *cpu, bool on, zenctl_err_t *err);

/* C-states */
int zenctl_cpu_get_cstate_disable(zenctl_cpu_t *cpu, int state, bool *out, zenctl_err_t *err);
int zenctl_cpu_set_cstate_disable(zenctl_cpu_t *cpu, int state, bool disable, zenctl_err_t *err);

#endif /* ZENCTL_CPU_H */
