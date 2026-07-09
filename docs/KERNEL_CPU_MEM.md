# Linux kernel CPU and memory control interfaces

A precise map of every sysfs, procfs, and (where relevant) syscall
interface the Linux kernel exposes for CPU and memory control. This is
the spec the Zenctl `lib/cpu` and `lib/mem` implementations code
against.

Conventions used below:

- **Path**: exact filesystem path. `N` is a logical CPU number
  (`0..NR_CPUS-1`). `X` is a policy number. `<size>` is a hugepage
  size in kB. `<N>` (in italics where shown) is a numeric value.
- **Read**: what a `cat` returns. Newline-terminated unless noted.
- **Write**: what an `echo` must send. Quoted strings must match
  exactly (case-insensitive for some files — noted where relevant).
- **Permissions**: octal mode, then `root to write, world-readable`
  when applicable. Sysfs files are owned `root:root` by default.
- **Units**: kHz, Hz, microseconds, milliseconds, nanoseconds, pages,
  bytes, kB, mB (milliwatts), percent.
- **Since**: kernel version the file was introduced in, when the ABI
  doc records one.
- **Quirks**: format gotchas the implementation must obey.

The canonical ABI reference is
`Documentation/ABI/{stable,testing}/sysfs-devices-system-cpu` and
`Documentation/ABI/stable/sysfs-devices-node` in the kernel source.
This document reflects Linux v6.20-rc1 (post-6.19 master as of
writing).

---

## CPU

### CPU policy and per-CPU layout

The `CPUFreq` core builds a `policyX` kobject under
`/sys/devices/system/cpu/cpufreq/policyX/` for every CPU-frequency
policy object. Each `/sys/devices/system/cpu/cpuN/` directory contains
a `cpufreq` *symbolic link* that points to the policy object that
governs CPU `N`. Reading and writing through either path hits the
same kobject and the same attributes — the per-CPU symlink is purely
for convenience. Multiple CPUs that share hardware P-state control
(they share `related_cpus`) point to the same `policyX`.

The implementation should access cpufreq attributes through the
per-CPU symlink (`/sys/devices/system/cpu/cpuN/cpufreq/...`) because
that path is stable across reboots and does not require the library to
discover the policy number.

### cpufreq

All files below live under
`/sys/devices/system/cpu/cpuN/cpufreq/` (which is a symlink to
`/sys/devices/system/cpu/cpufreq/policyX/`). They are created in
`drivers/cpufreq/cpufreq.c` and use the macros in
`include/linux/cpufreq.h`:

- `cpufreq_freq_attr_ro(_name)`     → mode `0444`
- `cpufreq_freq_attr_ro_perm(_name, _perm)` → custom mode
- `cpufreq_freq_attr_rw(_name)`     → mode `0644`
- `cpufreq_freq_attr_wo(_name)`     → mode `0200`

A read returns `-EBUSY` if the policy is inactive (all its CPUs are
offline). A write returns `-EBUSY` under the same condition.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_driver
- Read: name of the loaded cpufreq driver, e.g. `acpi-cpufreq`,
  `intel_pstate`, `intel_cpufreq`, `amd-pstate`, `cppc_cpufreq`.
- Write: not allowed.
- Permissions: `0444`.
- Units: N/A (string).
- Since: pre-git history.
- Quirks: trailing newline.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_governor
- Read: current governor name. Returns one of `performance`,
  `powersave` (when the driver uses `setpolicy`, i.e. `intel_pstate`
  active mode), or the name of the attached governor otherwise
  (`schedutil`, `ondemand`, `conservative`, `userspace`).
- Write: governor name. Must be one of the strings listed by
  `scaling_available_governors`. Parsing is done with `sscanf(buf,
  "%15s", str_governor)` — at most 15 characters, no whitespace,
  compared case-insensitively (`strncasecmp`) against the list of
  registered governors. If the name is not currently registered the
  core does `request_module("cpufreq_%s", str_governor)` and retries
  (governors can be modular).
- Permissions: `0644`.
- Units: N/A (string).
- Since: pre-git history.
- Quirks: writing `performance` or `powersave` to a `setpolicy`
  driver (e.g. `intel_pstate` active) selects the driver's internal
  policy. Writing any other governor in that mode returns `-EINVAL`.
  Writing any governor when no scaling driver is loaded returns
  `-EINVAL`.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_available_governors
- Read: space-separated list of registered governor names, with a
  trailing newline. For `setpolicy` drivers (intel_pstate active
  mode), returns the literal string `performance powersave\n`.
- Write: not allowed.
- Permissions: `0444`.
- Units: N/A (string).
- Since: pre-git history.
- Quirks: list may grow after `request_module` loads a new governor
  module.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_min_freq
- Read: minimum frequency the policy is allowed to run at, in **kHz**.
  `printf("%u\n", policy->min)`.
- Write: a string representing a non-negative integer, parsed with
  `kstrtoul(buf, 0, &val)` (base 0 means C-style — `0x` prefix is
  hex, leading `0` is octal, otherwise decimal; decimal is the safe
  choice). Must be `<= scaling_max_freq`. Enforced through
  `freq_qos_update_request()`; an out-of-range value returns the
  freq_qos error code (typically `-EINVAL` or `-ERANGE`).
- Permissions: `0644`.
- Units: **kHz**.
- Since: pre-git history.
- Quirks: writing a value `0` is accepted by the parser but rejected
  by the freq-QoS layer. Setting min above max returns `-EINVAL`.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_max_freq
- Read: maximum frequency the policy is allowed to run at, in **kHz**.
- Write: same rules as `scaling_min_freq`. Must be `>=
  scaling_min_freq`.
- Permissions: `0644`.
- Units: **kHz**.
- Since: pre-git history.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_setspeed
- Read: returns the last frequency requested by the `userspace`
  governor in kHz. If `userspace` is not the active governor or the
  governor does not implement `show_setspeed`, returns
  `<unsupported>\n`.
- Write: an unsigned integer frequency in kHz, parsed with
  `kstrtouint(buf, 0, &freq)`. Only effective when `userspace` is the
  active governor — otherwise returns `-EINVAL`. The actual frequency
  delivered may differ due to hardware coordination, thermal and power
  limits.
- Permissions: `0644`.
- Units: **kHz**.
- Since: pre-git history.
- Quirks: only meaningful with the `userspace` governor. The hardware
  may round to the nearest supported P-state.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq
- Read: current frequency of the policy. On x86 with
  `CONFIG_CPUFREQ_ARCH_CUR_FREQ=y` the value comes from
  `arch_freq_get_on_cpu()` (a hardware-feedback register like APERF/
  MPERF); otherwise the last value the driver requested
  (`policy->cur`). Value is in kHz.
- Write: not allowed.
- Permissions: `0444`.
- Units: **kHz**.
- Since: pre-git history.
- Quirks: may return the requested frequency, not the actual hardware
  frequency, depending on architecture. For the *measured* frequency
  use `cpuinfo_cur_freq` or `cpuinfo_avg_freq`.

#### /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_cur_freq
- Read: current frequency as obtained from the hardware via
  `__cpufreq_get()`. Returns `<unknown>\n` if the driver cannot read
  it.
- Write: not allowed.
- Permissions: **`0400`** (root-only, not world-readable). This is
  the only cpufreq attribute that uses a non-default RO mode.
- Units: **kHz**.
- Since: pre-git history.
- Quirks: not present on all drivers — the file is created
  unconditionally but the read may return `<unknown>`.

#### /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_avg_freq
- Read: average frequency over the last few milliseconds, derived
  from a hardware feedback register (ARM AMU, x86 APERF/MPERF).
  Returns `-EINVAL` (read returns error) if not supported, `-EAGAIN`
  on ARM if the CPU has been idle and the value cannot be computed.
- Write: not allowed.
- Permissions: `0444`.
- Units: **kHz**.
- Since: v5.6 (March 2020).
- Quirks: only present when `arch_freq_get_on_cpu()` is implemented
  (`!= -EOPNOTSUPP`).

#### /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_min_freq
- Read: hardware minimum operating frequency for this policy.
- Write: not allowed.
- Permissions: `0444`.
- Units: **kHz**.

#### /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq
- Read: hardware maximum operating frequency for this policy (includes
  turbo boost).
- Write: not allowed.
- Permissions: `0444`.
- Units: **kHz**.

#### /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_transition_latency
- Read: worst-case time to switch P-states.
- Write: not allowed.
- Permissions: `0444`.
- Units: **nanoseconds**. (Note the unit — *not* microseconds or
  kHz. Drivers report `policy->cpuinfo.transition_latency` which is
  in ns.)
- Since: pre-git history.
- Quirks: `CPUFREQ_ETERNAL` (`-1`) means unknown/unbounded.

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_available_frequencies
- Read: space-separated list of supported frequencies in kHz,
  newline-terminated. Format: `3400000 3200000 3000000 ...\n`. Each
  entry is `%u` from `pos->frequency` in the driver's frequency
  table.
- Write: not allowed.
- Permissions: `0444`.
- Units: **kHz**.
- Since: pre-git history.
- Quirks: only present when the driver supplies a frequency table
  (`policy->freq_table != NULL`). Boost frequencies are excluded from
  this list — they appear in `scaling_boost_frequencies` instead
  (when present).

#### /sys/devices/system/cpu/cpuN/cpufreq/scaling_boost_frequencies
- Read: space-separated list of boost-only frequencies in kHz.
- Write: not allowed.
- Permissions: `0444`.
- Units: **kHz**.
- Quirks: only present on drivers that distinguish boost entries in
  their frequency table (`CPUFREQ_BOOST_FREQ` flag).

#### /sys/devices/system/cpu/cpuN/cpufreq/related_cpus
- Read: space-separated list of *all* CPUs (online and offline) that
  share this policy's hardware P-state interface.
- Write: not allowed.
- Permissions: `0444`.
- Units: N/A (CPU numbers).

#### /sys/devices/system/cpu/cpuN/cpufreq/affected_cpus
- Read: space-separated list of *online* CPUs that share this policy.
  Subset of `related_cpus`.
- Write: not allowed.
- Permissions: `0444`.

#### /sys/devices/system/cpu/cpuN/cpufreq/bios_limit
- Read: maximum frequency limit imposed by the BIOS / service
  processor, in kHz. If the driver does not implement `bios_limit`,
  returns `cpuinfo.max_freq` instead.
- Write: not allowed.
- Permissions: `0444`.
- Units: **kHz**.
- Quirks: only present on drivers that implement `bios_limit`
  (e.g. some ACPI profiles).

#### /sys/devices/system/cpu/cpuN/cpufreq/freqdomain_cpus
- Read: CPUs that share the same clock/freq domain at the hardware
  level.
- Write: not allowed.
- Permissions: `0444`.
- Since: June 2013 (v3.11).
- Quirks: only present when `acpi-cpufreq` or `cppc-cpufreq` is in
  use. May differ from `related_cpus`.

#### /sys/devices/system/cpu/cpuN/cpufreq/boost
- Read: `0` or `1` — whether per-policy frequency boosting is enabled.
- Write: `0` or `1` (parsed with `kstrtobool`, so `n/y`, `0/1`,
  `off/on` all work). Only effective if the driver supports per-policy
  boost (`policy->boost_supported`) and global boost is already
  enabled.
- Permissions: `0644`.
- Units: N/A (boolean).
- Quirks: only present when the driver registers the local_boost
  attribute. Returns `-EINVAL` if global boost is off or the policy
  does not support boost.

#### /sys/devices/system/cpu/cpuN/cpufreq/energy_performance_preference
- Read: current Energy Performance Preference (EPP) string. One of:
  `default`, `performance`, `balance_performance`, `balance_power`,
  `power`.
- Write: one of the strings above (matched against
  `energy_perf_strings[]`), or a raw integer `0..255` (parsed with
  `kstrtouint(buf, 10, &epp)` — base 10 only). The raw form requires
  `X86_FEATURE_HWP_EPP`. Writing EPP > 0 while the policy is
  `performance` returns `-EBUSY`.
- Permissions: `0644`.
- Units: N/A (string) or 0..255 (raw).
- Since: v4.10 (December 2016).
- Quirks: **only present when the `intel_pstate` driver is in
  use** (active or passive). `amd-pstate` exposes its own equivalent
  file with the same name and a similar set of strings. Returns
  `-EAGAIN` if the driver is being re-registered.

#### /sys/devices/system/cpu/cpuN/cpufreq/energy_performance_available_preferences
- Read: space-separated list of valid EPP strings:
  `default performance balance_performance balance_power power\n`.
- Write: not allowed.
- Permissions: `0444`.
- Since: v4.10.
- Quirks: only present when `intel_pstate` or `amd-pstate` is in use.

#### /sys/devices/system/cpu/cpuN/cpufreq/auto_select
- Read: `0` or `1` — autonomous selection status.
- Write: `y`/`1`/`on` to enable, `n`/`0`/`off` to disable.
- Permissions: `0644`.
- Since: v6.14 (May 2025).
- Quirks: only present when `cppc-cpufreq` is in use.

#### /sys/devices/system/cpu/cpuN/cpufreq/auto_act_window
- Read: autonomous activity window in microseconds (max significand
  127).
- Write: integer microseconds, or `0` to let the platform choose.
  Values `> 130` are silently truncated to a 2-digit significand
  (e.g. `128` becomes `127`).
- Permissions: `0644`.
- Since: v6.14 (May 2025).
- Quirks: only meaningful when `auto_select` is enabled. Only
  present with `cppc-cpufreq`.

#### /sys/devices/system/cpu/cpuN/cpufreq/energy_performance_preference_val
- Read/write an 8-bit integer (`0..0xFF`). `0` = performance,
  `0xFF` = energy efficiency.
- Permissions: `0644`.
- Since: v6.14 (May 2025).
- Quirks: cppc-cpufreq only. Only meaningful when `auto_select` is
  enabled.

#### /sys/devices/system/cpu/cpuN/cpufreq/perf_limited
- Read: non-zero if platform throttling occurred.
- Write: bitmask to clear throttling status bits: `0x1` clears
  desired-performance excursion, `0x2` clears minimum-performance
  excursion, `0x3` clears both.
- Permissions: `0644`.
- Since: v6.15 (February 2026).
- Quirks: cppc-cpufreq only. OSPM may only *clear* bits; the
  platform sets them.

#### /sys/devices/system/cpu/cpuN/cpufreq/stats/  (subdirectory)
Only present when `CONFIG_CPU_FREQ_STAT=y`. Files inside are
read-only except `reset`. Created in
`drivers/cpufreq/cpufreq_stats.c`.

##### /sys/devices/system/cpu/cpuN/cpufreq/stats/time_in_state
- Read: one line per supported frequency: `<freq_kHz> <time_10ms>`.
  Time unit is 10 ms (same as `/proc/stat` jiffies-division).
- Permissions: `0444`.
- Units: kHz, 10 ms ticks.

##### /sys/devices/system/cpu/cpuN/cpufreq/stats/total_trans
- Read: total number of frequency transitions since boot or last
  reset.
- Permissions: `0444`.

##### /sys/devices/system/cpu/cpuN/cpufreq/stats/trans_table
- Read: 2-D transition-count matrix. First row is the "From : To"
  header with frequencies in kHz; each subsequent row is
  `<from_freq: <counts...>`. Returns `-EFBIG` if the table is larger
  than `PAGE_SIZE`.
- Permissions: `0444`.

##### /sys/devices/system/cpu/cpuN/cpufreq/stats/reset
- Read: not allowed.
- Write: any value (write triggers reset of all counters).
- Permissions: `0200` (write-only).
- Quirks: file appears as `--w-------` in `ls -l`.

### Per-governor tunables

Governors that take tunables expose them in a subdirectory named
after the governor. The location depends on the governor
implementation:

- **Per-policy** (modern, the default): the tunables live under
  `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/`.
- **Global**: tunables live under
  `/sys/devices/system/cpu/cpufreq/<gov>/` (rare today).

The implementation should look for `<gov>/` as a subdirectory of the
per-CPU `cpufreq` symlink target first, then fall back to the global
location.

All governor attributes use `__ATTR_RW(name)` (mode `0644`) or
`__ATTR_RO(name)` (mode `0444`). Integer values are parsed with
`kstrtouint`/`kstrtoint` (base 10) and printed with `sysfs_emit("%u",
val)`.

#### Governor: `performance`
No tunables. When attached, requests the policy's maximum frequency
within `scaling_max_freq`.

#### Governor: `powersave`
No tunables. When attached, requests the policy's minimum frequency
within `scaling_min_freq`.

#### Governor: `userspace`
No tunables. The user controls the frequency via
`scaling_setspeed` (see above).

#### Governor: `schedutil`
Located in `kernel/sched/cpufreq_schedutil.c`. Per-policy tunable:

##### /sys/devices/system/cpu/cpuN/cpufreq/schedutil/rate_limit_us
- Read: minimum microseconds between two governor computations.
  Default is `cpufreq_policy_transition_delay_us(policy)`, which is
  ~1.5× the transition latency or 1 ms if the driver has no latency.
- Write: unsigned integer microseconds, base 10.
- Permissions: `0644`.
- Units: **microseconds**.

#### Governor: `ondemand`
Located in `drivers/cpufreq/cpufreq_ondemand.c`. Per-policy
tunables:

##### /sys/devices/system/cpu/cpuN/cpufreq/ondemand/sampling_rate
- Read: sampling interval in microseconds. Default is
  `cpuinfo_transition_latency * 1.5`; minimum is two scheduler ticks.
- Write: unsigned int microseconds. Range-checked against
  `MIN_SAMPLING_RATE`.
- Permissions: `0644`.
- Units: **microseconds**.

##### /sys/devices/system/cpu/cpuN/cpufreq/ondemand/up_threshold
- Read: load percentage above which the governor jumps to maximum
  frequency. Default `95` (or `100` for `MICRO_FREQUENCY_UP_THRESHOLD`
  on systems with very high tick frequency).
- Write: integer percent, range `1..100`.
- Permissions: `0644`.
- Units: percent.

##### /sys/devices/system/cpu/cpuN/cpufreq/ondemand/ignore_nice_load
- Read: `0` or `1`.
- Write: `0` or `1`. When `1`, nice-time is treated as idle.
- Permissions: `0644`.

##### /sys/devices/system/cpu/cpuN/cpufreq/ondemand/sampling_down_factor
- Read: multiplier `1..100000`, default `1`.
- Write: integer in that range. Delays the next sampling after the
  CPU goes to max frequency.
- Permissions: `0644`.

##### /sys/devices/system/cpu/cpuN/cpufreq/ondemand/powersave_bias
- Read: reduction factor `0..1000`, default `0` (or `400` if the AMD
  sensitivity driver is loaded).
- Write: integer in that range.
- Permissions: `0644`.

##### /sys/devices/system/cpu/cpuN/cpufreq/ondemand/io_is_busy
- Read: `0` or `1`.
- Write: `0` or `1`. When `1`, I/O wait time is counted as busy.
- Permissions: `0644`.

#### Governor: `conservative`
Located in `drivers/cpufreq/cpufreq_conservative.c`. Per-policy
tunables:

##### /sys/devices/system/cpu/cpuN/cpufreq/conservative/sampling_rate
- Same semantics as `ondemand/sampling_rate`.
- Units: **microseconds**.

##### /sys/devices/system/cpu/cpuN/cpufreq/conservative/up_threshold
- Read: load percent above which frequency steps up. Default `75`.
  Range `1..100`, must be `> down_threshold`.
- Write: integer percent.
- Permissions: `0644`.

##### /sys/devices/system/cpu/cpuN/cpufreq/conservative/down_threshold
- Read: load percent below which frequency steps down. Default `20`.
  Range `1..100`, must be `< up_threshold`.
- Write: integer percent.
- Permissions: `0644`.

##### /sys/devices/system/cpu/cpuN/cpufreq/conservative/sampling_down_factor
- Read: integer `1..10`, default `1`.
- Write: integer in that range.
- Permissions: `0644`.

##### /sys/devices/system/cpu/cpuN/cpufreq/conservative/freq_step
- Read: percentage of `scaling_max_freq` to step per change, default
  `5`. Range `0..100`.
- Write: integer percent. `0` disables changes; `100` makes it
  ping-pong between min and max.
- Permissions: `0644`.

##### /sys/devices/system/cpu/cpuN/cpufreq/conservative/ignore_nice_load
- Same semantics as `ondemand/ignore_nice_load`.

### Global cpufreq controls

#### /sys/devices/system/cpu/cpufreq/boost
- Read: `0` or `1` — global boost (Turbo Boost / Core Performance
  Boost) enabled.
- Write: `0` or `1` (`kstrtobool`).
- Permissions: `0644`.
- Since: August 2012 (v3.7).
- Quirks: not present if the driver does not support boost, or if
  the driver exposes boost through a driver-specific interface
  (intel_pstate exposes `no_turbo` instead — see below).

#### /sys/devices/system/cpu/intel_pstate/status
- Read: `off`, `active`, or `passive`.
- Write: `off`, `active`, or `passive`. Switches the intel_pstate
  driver mode at runtime. `off` returns `-EBUSY` if HWP is active
  (cannot disable HWP once running).
- Permissions: `0644`.
- Quirks: only present when `intel_pstate` is the loaded driver.
  The string comparison uses `strncmp` against the exact length, so
  a trailing newline is fine but extra characters are not.

#### /sys/devices/system/cpu/intel_pstate/no_turbo
- Read: `0` or `1` — `1` means turbo frequencies are disabled.
- Write: integer `0` or `1` (parsed with `sscanf("%u")` — accepts
  any non-negative integer, then clamped to `0..1`). Writing `0`
  while the BIOS has disabled turbo returns `-EPERM`.
- Permissions: `0644`.

#### /sys/devices/system/cpu/intel_pstate/max_perf_pct
- Read: maximum P-state as a percentage of available performance,
  `0..100`.
- Write: unsigned int percent. Clamped to `[min_perf_pct, 100]`.
- Permissions: `0644`.
- Quirks: only present when intel_pstate is loaded *and* not using
  per-CPU limits (`per_cpu_limits == false`).

#### /sys/devices/system/cpu/intel_pstate/min_perf_pct
- Read: minimum P-state as a percentage, `0..100`.
- Write: unsigned int percent. Clamped to `[min_perf_pct_min(),
  max_perf_pct]`.
- Permissions: `0644`.

#### /sys/devices/system/cpu/intel_pstate/turbo_pct
- Read: percentage of total P-states that are turbo states.
- Permissions: `0444`.
- Quirks: not present on hybrid Intel CPUs (single file skipped
  when `X86_FEATURE_HYBRID_CPU`).

#### /sys/devices/system/cpu/intel_pstate/num_pstates
- Read: total number of P-states (turbo + non-turbo).
- Permissions: `0444`.
- Quirks: same hybrid-CPU exclusion as `turbo_pct`.

#### /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost
- Read: `0` or `1`.
- Write: unsigned int (any non-zero enables). Calls
  `intel_pstate_update_policies()`.
- Permissions: `0644`.
- Since: v4.18.
- Quirks: only exposed when HWP is active
  (`hwp_active == true`). Hidden otherwise.

#### /sys/devices/system/cpu/intel_pstate/energy_efficiency
- Read: `0` or `1` — `1` is the *inverted* value of the
  energy-efficiency bit in `MSR_IA32_POWER_CTL` (writing `1` enables
  EE; reading returns `!msr_bit`).
- Write: `0` or `1` (`kstrtobool`).
- Permissions: `0644`.
- Quirks: only on CPUs where the EE bit is meaningful. Not present
  on all Intel CPUs.

### cpuidle

The `CPUIdle` subsystem exposes its controls under two locations:

- `/sys/devices/system/cpu/cpuidle/` — global driver and governor
  selection.
- `/sys/devices/system/cpu/cpuN/cpuidle/state<M>/` — per-CPU,
  per-state attributes.

The global files are created by `drivers/cpuidle/sysfs.c`:

#### /sys/devices/system/cpu/cpuidle/available_governors
- Read: space-separated list of registered cpuidle governors
  (`menu`, `teo`, `ladder`, `haltpoll` depending on config).
- Permissions: `0444`.

#### /sys/devices/system/cpu/cpuidle/current_driver
- Read: name of the loaded cpuidle driver (`intel_idle`, `acpi_idle`,
  …).
- Permissions: `0444`.

#### /sys/devices/system/cpu/cpuidle/current_governor
- Read: name of the active cpuidle governor.
- Write: name of a governor from `available_governors`. Switches
  governor at runtime.
- Permissions: `0644`.

#### /sys/devices/system/cpu/cpuidle/current_governor_ro
- Read: same as `current_governor`.
- Write: not allowed.
- Permissions: `0444`.
- Quirks: present for kernels built with
  `CONFIG_CPU_IDLE_MULTIPLE_DRIVERS` or where the governor is
  read-only.

#### /sys/devices/system/cpu/cpuidle/intel_c1_demotion
- Read: `0` or `1`.
- Write: `0` or `1`. Toggles Intel C1 demotion hint.
- Permissions: `0644`.
- Quirks: Intel-only; only present when the intel_idle driver
  exposes it.

### Per-CPU, per-state cpuidle files

Each online CPU has a directory
`/sys/devices/system/cpu/cpuN/cpuidle/state<M>/` for each idle state
`M` the driver exposes. State `0` is the shallowest; higher `M` is
deeper. Created in `drivers/cpuidle/sysfs.c`. All state attributes use
`define_one_state_ro` (`0444`) except `disable` which uses
`define_one_state_rw` (`0644`).

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/name
- Read: short name of the idle state (e.g. `C1`, `C1E`, `C6`,
  `POLL`).
- Permissions: `0444`.
- Units: N/A (string).
- Since: v2.6.24 (September 2007).

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/desc
- Read: longer human-readable description.
- Permissions: `0444`.
- Since: v2.6.25 (February 2008).

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/latency
- Read: worst-case exit latency.
- Permissions: `0444`.
- Units: **microseconds**.
- Since: v2.6.24.
- Quirks: named `latency` in sysfs but maps to `exit_latency_ns` in
  the kernel struct (`ktime_to_us()` is applied on read).

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/residency
- Read: target residency — minimum time the CPU should stay in this
  state to make the transition worthwhile.
- Permissions: `0444`.
- Units: **microseconds**.
- Since: v3.15 (March 2014).
- Quirks: sysfs name is `residency`, internal field is
  `target_residency_ns`.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/power
- Read: power consumed while in this state.
- Permissions: `0444`.
- Units: **milliwatts**.
- Since: v2.6.24.
- Quirks: often `0` because the value is not well-defined for
  hierarchical idle states.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/usage
- Read: number of times this state has been requested.
- Permissions: `0444`.
- Units: count (`unsigned long long`).
- Since: v2.6.24.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/rejected
- Read: number of times a request to enter this state was rejected
  (typically because an interrupt arrived during entry).
- Permissions: `0444`.
- Units: count.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/time
- Read: total time spent in this state.
- Permissions: `0444`.
- Units: **microseconds** (`ktime_to_us(state_usage->time_ns)`).
- Since: v2.6.24.
- Quirks: may overcount when the hardware enters a shallower state
  than requested.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/above
- Read: number of times this state was entered but the actual idle
  duration was *shorter* than this state's target residency (a deeper
  state should have been chosen — counter-intuitively named).
- Permissions: `0444`.
- Units: count.
- Since: v4.18 (April 2018).

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/below
- Read: number of times this state was entered but the actual idle
  duration was *longer* than the next-deeper state's target residency
  (a deeper state would have saved more energy).
- Permissions: `0444`.
- Units: count.
- Since: v4.18.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/disable
- Read: `0` (enabled) or `1` (disabled). Reads
  `state_usage->disable & CPUIDLE_STATE_DISABLED_BY_USER`.
- Write: unsigned integer (`kstrtouint` with base 0 — `0`, `1`, or
  any non-zero integer which is treated as `1`). Requires
  `CAP_SYS_ADMIN`; otherwise returns `-EPERM`. Non-zero sets the
  `CPUIDLE_STATE_DISABLED_BY_USER` bit; zero clears it.
- Permissions: `0644` (and the writer must have `CAP_SYS_ADMIN`).
- Since: v3.10 (March 2012).
- Quirks: writing a non-zero value only disables the state for
  *this CPU*. The same state on other CPUs is unaffected; to disable
  a state system-wide you must write to every CPU's `disable` file.
  In the `ladder` governor, disabling a lighter state also disables
  deeper states (the disable bit is not propagated consistently),
  while in `menu`/`teo` only the named state is excluded.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/default_status
- Read: `enabled` or `disabled` — the driver's compile- or boot-time
  default for this state.
- Permissions: `0444`.
- Since: v5.6 (December 2019).
- Quirks: reflects `CPUIDLE_FLAG_OFF` in the state's flags.

#### /sys/devices/system/cpu/cpuN/cpuidle/stateM/s2idle/
Subdirectory present only when the state can be used in
suspend-to-idle (s2idle) with suspended timekeeping. Created in v4.17
(March 2018).

##### /sys/devices/system/cpu/cpuN/cpuidle/stateM/s2idle/usage
- Read: number of times this state was requested during s2idle.
- Permissions: `0444`.

##### /sys/devices/system/cpu/cpuN/cpuidle/stateM/s2idle/time
- Read: total time spent in s2idle for this state, in microseconds.
- Permissions: `0444`.

### CPU online/offline

#### /sys/devices/system/cpu/cpuN/online
- Read: `0` (offline) or `1` (online). Format: `%u\n`.
- Write: a boolean (`kstrtobool` accepts `0`, `1`, `n`, `y`, `off`,
  `on`). `1` brings the CPU online via `device_online()`, `0` takes
  it offline via `device_offline()`.
- Permissions: `0644`. Implemented by the device core in
  `drivers/base/core.c` as `DEVICE_ATTR_RW(online)`, *not* by the
  CPU driver — the same attribute exists on every hotpluggable
  device.
- Since: December 2008.
- Quirks: file is only present when the CPU is hotpluggable
  (`cpu->hotpluggable == true`). CPU 0 is typically *not*
  hotpluggable; on x86 it has no `online` file. The file is created
  per-CPU by `register_cpu()` in `drivers/base/cpu.c`. Returns
  `-EBUSY` if the hotplug operation fails (e.g. trying to offline a
  CPU with bound unmovable workqueues). Write requires
  `CAP_SYS_ADMIN`.

### CPU mask files (top-level)

Created in `drivers/base/cpu.c`. All are `0444` (`DEVICE_ATTR`).
Each returns a CPU list in the format `0-3,8-11,14,17\n`.

#### /sys/devices/system/cpu/online
CPUs that are online and being scheduled.

#### /sys/devices/system/cpu/possible
CPUs that have been allocated resources and could be brought online.

#### /sys/devices/system/cpu/present
CPUs that have been identified by the firmware.

#### /sys/devices/system/cpu/offline
CPU-list complement of `online` within `possible`. Includes any CPU
indices `>= nr_cpu_ids` that the firmware reported.

#### /sys/devices/system/cpu/enabled
- Read: CPU list of CPUs that can be brought online (post
  `nosmt`/`mitigations=off` boot parameters).
- Permissions: `0444`.
- Since: November 2022 (v6.2).

#### /sys/devices/system/cpu/kernel_max
- Read: `NR_CPUS - 1` — the maximum CPU index the kernel was
  configured with.
- Permissions: `0444`.

#### /sys/devices/system/cpu/isolated
- Read: CPU list of CPUs isolated from scheduler load balancing
  (`isolcpus=` boot parameter).
- Permissions: `0444`.

#### /sys/devices/system/cpu/nohz_full
- Read: CPU list of full NOHZ (tickless) CPUs (`nohz_full=` boot
  parameter).
- Permissions: `0444`.

#### /sys/devices/system/cpu/housekeeping
- Read: CPU list of housekeeping CPUs.
- Permissions: `0444`.
- Since: v6.16 (October 2025).

### SMT control

Implemented in `kernel/cpu.c`. The `smt` attribute group is created
under `/sys/devices/system/cpu/smt/` by `cpu_smt_sysfs_init()`.

#### /sys/devices/system/cpu/smt/control
- Read: current SMT control state. One of:
  - `on` — SMT is enabled (all sibling threads online).
  - `off` — SMT is disabled.
  - `<N>` — SMT is enabled with `N` threads per core (where `N` is
    not the maximum; shows the active thread count as a decimal).
  - `forceoff` — SMT is force-disabled; cannot be re-enabled.
  - `notsupported` — the CPU does not support SMT.
  - `notimplemented` — the architecture does not implement runtime
    SMT toggling.
- Write: one of `on`, `off`, `forceoff`, or an integer thread count
  (`kstrtoint(buf, 10, ...)`). Writes are accepted only when the
  current state is *not* `forceoff` (returns `-EPERM`) or
  `notsupported` (returns `-ENODEV`). Integer `1` is treated as
  `off`; integers `>=2` are validated against
  `cpu_smt_num_threads_valid()` (which on `SMT_NUM_THREADS_DYNAMIC`
  accepts `1..cpu_smt_max_threads`, otherwise accepts only `1` and
  `cpu_smt_max_threads`).
- Permissions: `0644`.
- Since: June 2018 (v4.18 — introduced for L1TF / Foreshadow
  mitigation).
- Quirks: writing `forceoff` is one-way. After `forceoff` the file
  becomes read-only (`-EPERM` on write). On PowerPC, enabling SMT
  silently skips offline cores. Disabling SMT also offlines sibling
  threads; it does not offline primary threads.

#### /sys/devices/system/cpu/smt/active
- Read: `0` or `1` — whether SMT siblings are currently online and
  active (`sched_smt_active()`).
- Permissions: `0444`.

### Microcode

Implemented in `arch/x86/kernel/cpu/microcode/core.c` (x86 only;
other architectures have their own loaders).

Two attribute groups share the name `microcode`:

1. **Per-CPU group** (`mc_attr_group`) attached under
   `/sys/devices/system/cpu/cpuN/microcode/`. Contains:
   - `version` — `0444`. Read returns `0x%x\n` (hex with `0x`
     prefix) — the loaded microcode revision.
   - `processor_flags` — `0444`. Read returns `0x%x\n` — the
     processor flags field from the microcode signature.
2. **Global group** (`cpu_root_microcode_group`) attached under
   `/sys/devices/system/cpu/microcode/`. Contains only `reload`
   (when `CONFIG_MICROCODE_LATE_LOADING=y`).

#### /sys/devices/system/cpu/cpuN/microcode/version
- Read: hex string with `0x` prefix, e.g. `0x0a201016\n`.
- Write: not allowed.
- Permissions: `0444`.
- Units: N/A (microcode revision, architecture-specific meaning).

#### /sys/devices/system/cpu/cpuN/microcode/processor_flags
- Read: `0x%x\n`.
- Write: not allowed.
- Permissions: `0444`.

#### /sys/devices/system/cpu/microcode/reload
- Read: not allowed.
- Write: **only the literal `1`** is accepted. `kstrtoul(buf, 0,
  &val)` parses the value and the store then checks `val != 1` —
  anything else (including `0`, `2`, `true`) returns `-EINVAL`. The
  write triggers a late microcode reload across all CPUs.
- Permissions: `0200` (write-only).
- Quirks: file only exists when `CONFIG_MICROCODE_LATE_LOADING=y`.
  Late loading is *dangerous* and may hang the system; the kernel
  may also refuse if any CPU is in a state that prevents reload.
  Vendor code (Intel/AMD) may further restrict when the reload can
  happen.

### Topology

Created in `drivers/base/topology.c`. The attribute group is created
per CPU. All files are `0444` (`DEVICE_ATTR_RO`) unless noted. The
`*_list` files use `cpumap_print_to_pagebuf(false, ...)` (decimal CPU
list); the non-`_list` siblings use `cpumap_print_to_pagebuf(true,
...)` (hex bitmask).

#### /sys/devices/system/cpu/cpuN/topology/physical_package_id
- Read: physical socket/package number, signed integer.
- Permissions: `0444`.

#### /sys/devices/system/cpu/cpuN/topology/die_id
- Read: die ID within the package. Multi-die packages have `>0`
  values.
- Permissions: `0444`.

#### /sys/devices/system/cpu/cpuN/topology/cluster_id
- Read: cluster ID.
- Permissions: `0444`.

#### /sys/devices/system/cpu/cpuN/topology/core_id
- Read: core ID within the package.
- Permissions: `0444`.

#### /sys/devices/system/cpu/cpuN/topology/ppin
- Read: Protected Processor Identification Number (per-socket
  unique). Format `0x%llx\n`.
- Permissions: `0400` (`DEVICE_ATTR_ADMIN_RO` — root-only).
- Quirks: file appears as `-r--------`. Only present on CPUs that
  expose a PPIN (Intel Xeon, some AMD). May be suppressed by BIOS.

#### /sys/devices/system/cpu/cpuN/topology/thread_siblings
- Read: hex bitmask of CPUs in the same core (hardware threads on
  the same physical core).
- Permissions: `0444` (bin attribute).
- Quirks: deprecated name; the new name is `core_cpus`. Both exist
  on x86 for backwards compatibility.

#### /sys/devices/system/cpu/cpuN/topology/thread_siblings_list
- Read: decimal CPU list of siblings in the same core, e.g.
  `0,2\n`.
- Permissions: `0444` (bin attribute).
- Quirks: deprecated alias for `core_cpus_list`.

#### /sys/devices/system/cpu/cpuN/topology/core_siblings
- Read: hex bitmask of CPUs in the same physical package.
- Permissions: `0444`.
- Quirks: deprecated alias for `package_cpus`.

#### /sys/devices/system/cpu/cpuN/topology/core_siblings_list
- Read: decimal CPU list of CPUs in the same physical package.
- Permissions: `0444`.
- Quirks: deprecated alias for `package_cpus_list`.

#### /sys/devices/system/cpu/cpuN/topology/core_cpus, core_cpus_list, package_cpus, package_cpus_list, die_cpus, die_cpus_list, cluster_cpus, cluster_cpus_list, book_siblings, book_siblings_list, drawer_siblings, drawer_siblings_list
New names for the deprecated files above. `book_*` and `drawer_*`
are s390-only. Format identical to their deprecated counterparts.

#### /sys/devices/system/cpu/cpuN/cpu_capacity
- Read: capacity of this CPU (a relative compute-capacity number,
  scheduler-visible). Symmetric CPUs report `1024`. Format:
  `%llu\n`.
- Permissions: `0444`.
- Since: December 2016 (v4.10).
- Quirks: only present on architectures that register
  `cpu_capacity` (ARM/ARM64 with DT/ACPI topology).

### Other CPU files of interest

Not strictly needed for the CPU domain implementation but
discoverable in the same directory; the library may surface them
read-only for completeness.

#### /sys/devices/system/cpu/cpuN/cache/indexK/
Per-cache (level K) attributes: `level`, `type`, `size`,
`coherency_line_size`, `number_of_sets`, `physical_line_partition`,
`shared_cpu_list`, `shared_cpu_map`, `allocation_policy`,
`write_policy`, `id`, `ways_of_associativity`. All `0444`. `size`
is in kB. `shared_cpu_list` is a decimal CPU list.

#### /sys/devices/system/cpu/vulnerabilities/<NAME>
Per-CPU-architecture vulnerability mitigations. Each file is `0444`
and returns `Not affected`, `Vulnerable`, or `Mitigation: <desc>`.
Names include `meltdown`, `spectre_v1`, `spectre_v2`,
`spec_store_bypass`, `l1tf`, `mds`, `tsx_async_abort`,
`itlb_multihit`, `srbds`, `retbleed`, `mmio_stale_data`,
`gather_data_sampling`, `reg_file_data_sampling`, `old_microcode`,
`indirect_target_selection`, `tsa`, `vmscape`, `ghostwrite`,
`spec_rstack_overflow`. Located under
`/sys/devices/system/cpu/vulnerabilities/`.

#### /sys/devices/system/cpu/cpuN/power/energy_perf_bias
- Read: integer `0..15` (Intel EPB hint, 0 = highest performance,
  15 = max energy savings).
- Write: integer `0..15` or one of `performance`,
  `balance-performance`, `normal`, `balance-power`, `power`.
- Permissions: `0644`.
- Since: March 2019 (v5.1).
- Quirks: only present on Intel CPUs that support EPB. Not the same
  thing as EPP — EPB predates HWP and is an OS-level hint; EPP is an
  HWP register field.

---

## Memory

### Hugepages (static, hugetlb)

Two parallel interfaces exist: the legacy `/proc/sys/vm/` sysctls
(which control only the *default* hugepage size) and the newer
`/sys/kernel/mm/hugepages/` sysfs tree (which controls every
supported size). The implementation should use the sysfs tree for
multi-size support and fall back to the sysctls for the default size
only.

#### /proc/sys/vm/nr_hugepages
- Read: number of persistent huge pages of the *default* size
  currently in the pool, as `unsigned long`.
- Write: unsigned long, decimal, parsed via
  `proc_doulongvec_minmax`. Adjusts the pool up or down. The actual
  allocation is distributed across all allowed NUMA nodes per the
  calling task's memory policy (mempolicy is *ignored* when writing
  to this file).
- Permissions: `0644`.
- Units: **pages** (of the default hugepage size, e.g. 2048 KiB on
  x86-64).
- Quirks: allocation can fail silently on a node with insufficient
  contiguous memory; the kernel then attempts other nodes. Use
  `nr_hugepages_mempolicy` if you want strict per-node allocation
  honoring a mempolicy.

#### /proc/sys/vm/nr_hugepages_mempolicy
- Read: same as `nr_hugepages` (returns the global pool count).
- Write: same format as `nr_hugepages`, but the allocation/free
  honors the calling task's NUMA memory policy. The recommended
  invocation is:
  ```
  numactl --interleave <node-list> echo 20 \
      > /proc/sys/vm/nr_hugepages_mempolicy
  ```
- Permissions: `0644`.
- Units: **pages**.
- Quirks: only present when `CONFIG_NUMA=y`.

#### /proc/sys/vm/nr_overcommit_hugepages
- Read: maximum number of "surplus" huge pages the kernel is
  allowed to allocate from the normal page pool when the persistent
  pool is exhausted.
- Write: unsigned long. Returns `-EINVAL` if the default hstate is
  gigantic (gigantic pages cannot be overcommitted at runtime).
- Permissions: `0644`.
- Units: **pages**.

#### /proc/sys/vm/hugetlb_shm_group
- Read: GID that is allowed to use `SHM_HUGETLB` for SysV shared
  memory without root privileges.
- Write: integer GID.
- Permissions: `0644`.
- Units: N/A (gid_t).

#### /proc/sys/vm/movable_gigantic_pages
- Read: `0` or `1` — whether gigantic pages can be migrated off
  ZONE_MOVABLE.
- Permissions: `0644`.
- Quirks: only present when
  `CONFIG_ARCH_ENABLE_HUGEPAGE_MIGRATION=y`.

### /sys/kernel/mm/hugepages/hugepages-<size>kB/

Per-size hugepage controls. One subdirectory per supported size.
`<size>` is the page size in kB (e.g. `2048`, `1048576` on x86-64
for 2 MiB and 1 GiB). All files use `HSTATE_ATTR` macros which map
to `__ATTR_RW` (`0644`), `__ATTR_RO` (`0444`), or `__ATTR_WO`
(`0200`).

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/nr_hugepages
- Read: total number of `<size>` huge pages in the pool,
  `%lu\n`.
- Write: unsigned long, decimal. Adjusts the pool; mempolicy is
  *ignored*. The kernel distributes across all online memory nodes
  and may over-allocate on nodes with sufficient contiguous memory
  to make up for failures elsewhere.
- Permissions: `0644`.
- Units: **pages**.

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/nr_hugepages_mempolicy
- Read: same as `nr_hugepages`.
- Write: same format, but honors the calling task's mempolicy.
- Permissions: `0644`.
- Units: **pages**.
- Quirks: only present when `CONFIG_NUMA=y`.

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/nr_overcommit_hugepages
- Read: `%lu\n` — surplus limit.
- Write: unsigned long. `-EINVAL` for gigantic hstates that cannot
  be runtime-overcommitted.
- Permissions: `0644`.

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/free_hugepages
- Read: number of free huge pages of this size, `%lu\n`.
- Permissions: `0444`.

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/resv_hugepages
- Read: number of huge pages reserved (committed but not yet
  allocated), `%lu\n`.
- Permissions: `0444`.

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/surplus_hugepages
- Read: number of surplus (over-committed) huge pages currently in
  the pool, `%lu\n`.
- Permissions: `0444`.

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/demote_size
- Read: `<size>kB\n` — the size that pages will be demoted *into*
  (the next smaller supported size by default).
- Write: a size string parsed with `memparse` (accepts plain bytes
  or suffixes `K`, `M`, `G`, `k`, `m`, `g`). Must be a smaller
  supported hugepage size.
- Permissions: `0644`.
- Quirks: only present when the hstate has a smaller hstate to
  demote into (`h->demote_order != 0`).

#### /sys/kernel/mm/hugepages/hugepages-<size>kB/demote
- Read: not allowed.
- Write: unsigned long count of pages to demote. The kernel may
  demote fewer than requested (e.g. only `free_hugepages -
  resv_hugepages` are eligible). Compare `nr_hugepages` before and
  after to determine the actual demote count.
- Permissions: `0200` (write-only).
- Quirks: only present when `demote_size` is present. Not available
  for the smallest supported size.

### Per-node hugepage controls

Located under
`/sys/devices/system/node/nodeN/hugepages/hugepages-<size>kB/`. Each
subdirectory contains only:

- `nr_hugepages` — `0644`. Read returns per-node count; write sets
  the per-node pool size, *ignoring* mempolicy and cpuset
  constraints (subject to available contiguous memory).
- `free_hugepages` — `0444`. Per-node free count.
- `surplus_hugepages` — `0444`. Per-node surplus count.

Per-node `nr_overcommit_hugepages` and `resv_hugepages` are **not**
exposed — those remain global quantities.

### Transparent Huge Pages (THP)

THP controls live under `/sys/kernel/mm/transparent_hugepage/`. All
files use `__ATTR_RW` (`0644`) or `__ATTR_RO` (`0444`) via
`kobj_attribute`.

#### /sys/kernel/mm/transparent_hugepage/enabled
- Read: a single line with the three modes, the active one in
  square brackets: `[always] madvise never`,
  `always [madvise] never`, or `always madvise [never]`.
- Write: one of `always`, `madvise`, `never` (matched exactly with
  `sysfs_match_string`). Toggling this starts or stops `khugepaged`
  accordingly.
- Permissions: `0644`.
- Quirks: this is the *global* toggle for anonymous PMD-sized THP
  only. Multi-size THP (mTHP) is controlled per-size below.
  Setting `never` here does **not** disable `madvise(...,
  MADV_COLLAPSE)`.

#### /sys/kernel/mm/transparent_hugepage/hugepages-<size>kB/enabled
- Read: same format as the global `enabled`, but with the extra
  `inherit` mode: `[always] madvise never inherit`.
- Write: one of `always`, `madvise`, `never`, `inherit`. `inherit`
  means "use the global `enabled` value".
- Permissions: `0644`.
- Quirks: by default PMD-sized (`hugepages-2048kB` on x86-64) is
  `inherit`; all other sizes are `never`. Only anonymous mTHP sizes
  are exposed here.

#### /sys/kernel/mm/transparent_hugepage/defrag
- Read: the five defrag modes, active one in brackets:
  `[always] defer defer+madvise madvise never`.
- Write: one of `always`, `defer`, `defer+madvise`, `madvise`,
  `never` (matched with `sysfs_match_string` — `defer+madvise` is
  a single token, not two).
- Permissions: `0644`.
- Modes:
  - `always` — synchronous direct reclaim + compaction on every THP
    fault. Highest latency, best THP coverage.
  - `defer` — wake kswapd/kcompactd; rely on khugepaged later.
  - `defer+madvise` — `always` for `MADV_HUGEPAGE` regions;
    `defer` for everything else.
  - `madvise` — synchronous direct reclaim only for
    `MADV_HUGEPAGE` regions. **This is the default.**
  - `never` — fallback to small pages if a THP is not immediately
    available.

#### /sys/kernel/mm/transparent_hugepage/use_zero_page
- Read: `0` or `1` — whether the kernel uses a huge zero page for
  read-faults to anonymous mappings.
- Write: `0` or `1` (parsed with `kstrtoul`, base 10; only `0` and
  `1` accepted).
- Permissions: `0644`.

#### /sys/kernel/mm/transparent_hugepage/hpage_pmd_size
- Read: size of a PMD-mappable transparent hugepage in bytes, e.g.
  `2097152\n`.
- Write: not allowed.
- Permissions: `0444`.
- Units: **bytes**.

#### /sys/kernel/mm/transparent_hugepage/shrink_underused
- Read: `0` or `1` — whether underused THPs (more zero pages than
  `max_ptes_none` allows) are split under memory pressure.
- Write: `0` or `1`.
- Permissions: `0644`.

#### /sys/kernel/mm/transparent_hugepage/shmem_enabled
- Read: the six shmem THP modes, active one in brackets:
  `[always] within_size advise never deny force`.
- Write: one of `always`, `within_size`, `advise`, `never`, `deny`,
  `force` (parsed by `shmem_parse_huge`).
- Permissions: `0644`.
- Quirks: applies to the internal shmem/tmpfs mount (SysV SHM,
  memfds, MAP_ANONYMOUS|MAP_SHARED, DRM objects, ashmem).
  `deny` and `force` are emergency/testing controls that override
  per-mount `huge=` options.

#### /sys/kernel/mm/transparent_hugepage/hugepages-<size>kB/shmem_enabled
- Read: same format as the global `shmem_enabled`, but with the
  `inherit` mode (and without `deny`/`force` for individual sizes).
- Write: one of `always`, `inherit`, `never`, `within_size`,
  `advise`.
- Permissions: `0644`.

### khugepaged controls

Located under
`/sys/kernel/mm/transparent_hugepage/khugepaged/`. All files are
`0644` unless noted. The implementation should expose these as
read/write through the `mem` domain.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/defrag
- Read: `0` or `1` — whether khugepaged does direct compaction.
- Write: `0` or `1`.
- Permissions: `0644`.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan
- Read: number of pages khugepaged scans per pass, `%u\n`. Default
  `HPAGE_PMD_NR * 8` (e.g. `4096` on a 4 KiB base page system with
  2 MiB THP).
- Write: unsigned int. A write of `0` is rejected.
- Permissions: `0644`.
- Units: **pages** (small pages).

#### /sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed
- Read: cumulative count of pages collapsed by khugepaged.
- Permissions: `0444`.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/full_scans
- Read: number of full address-space scans completed.
- Permissions: `0444`.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs
- Read: milliseconds khugepaged sleeps between scan passes,
  `%u\n`. Default `10000`.
- Write: unsigned int milliseconds. `0` means run at 100% of one
  CPU.
- Permissions: `0644`.
- Units: **milliseconds**.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/alloc_sleep_millisecs
- Read: milliseconds khugepaged sleeps after a hugepage allocation
  failure, `%u\n`. Default `60000`.
- Write: unsigned int milliseconds.
- Permissions: `0644`.
- Units: **milliseconds**.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_none
- Read: maximum number of empty (none/zero) PTEs allowed in a
  candidate region for collapse, `%u\n`. Default
  `KHUGEPAGED_MAX_PTES_LIMIT` (= `HPAGE_PMD_NR - 1`).
- Write: unsigned long, `0..KHUGEPAGED_MAX_PTES_LIMIT`.
- Permissions: `0644`.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_swap
- Read: maximum number of swap PTEs allowed in a candidate region.
  Default `HPAGE_PMD_NR / 8`.
- Write: unsigned long, `0..KHUGEPAGED_MAX_PTES_LIMIT`.
- Permissions: `0644`.

#### /sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_shared
- Read: maximum number of shared PTEs allowed in a candidate
  region. Default `HPAGE_PMD_NR / 2`.
- Write: unsigned long, `0..KHUGEPAGED_MAX_PTES_LIMIT`.
- Permissions: `0644`.

### NUMA

The kernel exposes NUMA topology and statistics under
`/sys/devices/system/node/`. Memory *policy* is not exposed via
sysfs — it is set per-task via the `set_mempolicy(2)`, `mbind(2)`,
`get_mempolicy(2)`, and `set_mempolicy_home_node(2)` syscalls. The
`numactl(8)` user-space tool wraps those syscalls.

#### /sys/devices/system/node/online
- Read: CPU-list-formatted list of online NUMA nodes.
- Permissions: `0444`.

#### /sys/devices/system/node/possible
- Read: list of NUMA nodes that could possibly come online.
- Permissions: `0444`.

#### /sys/devices/system/node/has_normal_memory
- Read: list of nodes that have regular (ZONE_NORMAL or below)
  memory.
- Permissions: `0444`.

#### /sys/devices/system/node/has_cpu
- Read: list of nodes that contain one or more CPUs.
- Permissions: `0444`.

#### /sys/devices/system/node/has_high_memory
- Read: list of nodes that have high memory. Only present when
  `CONFIG_HIGHMEM=y` (32-bit kernels).
- Permissions: `0444`.

#### /sys/devices/system/node/nodeN/cpumap
- Read: hex CPU bitmask of CPUs local to node N.
- Permissions: `0444` (bin attribute).

#### /sys/devices/system/node/nodeN/cpulist
- Read: decimal CPU list of CPUs local to node N.
- Permissions: `0444` (bin attribute).

#### /sys/devices/system/node/nodeN/meminfo
- Read: per-node memory-info summary. Same field set as
  `/proc/meminfo`, prefixed with `Node N,`. Each value is in kB.
  Includes lines like:
  ```
  Node 0 MemTotal:       16384000 kB
  Node 0 MemFree:         8192000 kB
  Node 0 HugePages_Total:    100
  Node 0 HugePages_Free:      50
  Node 0 HugePages_Surp:       0
  ```
- Permissions: `0444`.
- Units: kB for memory; **pages** for HugePages fields (note: not
  kB unlike the rest of the file — this is a long-standing
  inconsistency).

#### /sys/devices/system/node/nodeN/numastat
- Read: per-node hit/miss counters in pages:
  ```
  numa_hit N
  numa_miss N
  numa_foreign N
  interleave_hit N
  local_node N
  other_node N
  ```
- Permissions: `0444`.
- Units: **pages**.

#### /sys/devices/system/node/nodeN/distance
- Read: space-separated list of NUMA distance values from this node
  to every other online node, e.g. `10 21 21 10\n`. Self-distance is
  typically `10` (the base); same-socket siblings are `10`–`21`;
  remote nodes are `>21`. ACPI SLIT-derived.
- Permissions: `0444`.
- Units: relative (dimensionless). `10` = local.

#### /sys/devices/system/node/nodeN/vmstat
- Read: superset of `numastat` — every per-zone VM stat counter
  plus per-node counters and NUMA-event counters, one `<name> <val>`
  pair per line.
- Permissions: `0444`.
- Units: pages.

#### /sys/devices/system/node/nodeN/compact
- Read: not allowed.
- Write: any value — the write triggers memory compaction on the
  node. The kernel drains per-CPU LRU lists and runs
  `compact_node()`.
- Permissions: `0200` (write-only).
- Quirks: write does not return until compaction completes; can be
  expensive on large nodes.

#### /sys/devices/system/node/nodeN/reclaim
- Read: not allowed.
- Write: any value — triggers user-requested proactive reclaim on
  the node. Equivalent to the memcg reclaim interface.
- Permissions: `0200`.
- Since: June 2025 (v6.16).

#### /sys/devices/system/node/nodeN/accessY/
NUMA access class subdirectories — for tiered-memory and CXL.
Contains `initiators/` and `targets/` subdirectories with symlinks
to related nodes, plus per-access-class bandwidth/latency files:

- `accessY/initiators/read_bandwidth` — MB/s.
- `accessY/initiators/read_latency` — nanoseconds.
- `accessY/initiators/write_bandwidth` — MB/s.
- `accessY/initiators/write_latency` — nanoseconds.

All `0444`. Since December 2018 (v5.0).

#### /sys/devices/system/node/nodeN/memory_side_cache/indexY/
Memory-side cache attributes for level Y. Files: `indexing` (0 =
direct-mapped, non-zero = indexed), `line_size` (bytes),
`size` (bytes), `write_policy` (0 = write-back, 1 = write-through),
`address_mode` (0 = reserved, 1 = extended-linear). All `0444`.

#### /sys/devices/system/node/nodeN/memory_failure/{total,ignored,failed,delayed,recovered}
Per-node memory-failure counters. All `0444`. Units: pages. Since
January 2023 (v6.2).

#### /sys/devices/system/node/nodeN/x86/sgx_total_bytes
- Read: total SGX EPC memory on this node in bytes.
- Permissions: `0444`.
- Quirks: x86-only, only when SGX is supported and initialized.

#### /sys/devices/system/node/nodeN/hugepages/hugepages-<size>kB/
See the "Per-node hugepage controls" section above.

### NUMA memory policy (syscalls, not sysfs)

The kernel exposes no sysfs for setting policy. Policy is set per
task via:

- `set_mempolicy(int mode, const unsigned long *nmask, unsigned
  long maxnode)` — set the calling task's task/process policy.
- `mbind(void *start, unsigned long len, int mode, const unsigned
  long *nmask, unsigned long maxnode, unsigned int flags)` —
  install a VMA policy on a range of the calling task's address
  space; with `MPOL_MF_MOVE`/`MPOL_MF_MOVE_ALL` it can also migrate
  existing pages to match the new policy.
- `get_mempolicy(int *mode, const unsigned long *nmask, unsigned
  long maxnode, void *addr, int flags)` — query the task policy
  or the policy/Node at a specific address.
- `set_mempolicy_home_node(unsigned long start, unsigned long len,
  unsigned long home_node, unsigned long flags)` — set the home
  Node for an existing VMA policy.

Supported modes (`include/uapi/linux/mempolicy.h`):

| Mode             | Meaning |
|------------------|---------|
| `MPOL_DEFAULT`   | Fall back to the next more general scope. |
| `MPOL_BIND`      | Allocate from the listed nodes, closest first. |
| `MPOL_PREFERRED` | Allocate from the listed node; fall back to others. Empty mask = local. |
| `MPOL_INTERLEAVE`| Round-robin page allocations across the listed nodes. |
| `MPOL_LOCAL`     | Allocate from the Node the CPU is on. |
| `MPOL_PREFERRED_MANY` | Like PREFERRED but a set of nodes (v5.15+). |

Mode flags:
- `MPOL_F_STATIC_NODES` — nodemask is absolute (not remapped on
  cpuset rebind).
- `MPOL_F_RELATIVE_NODES` — nodemask is relative to the cpuset's
  allowed nodes.

`mbind` flags (`flags` argument):
- `MPOL_MF_STRICT` — fail if any existing pages violate the new
  policy.
- `MPOL_MF_MOVE` — migrate existing private pages to match the
  policy.
- `MPOL_MF_MOVE_ALL` — also migrate shared pages (requires
  `CAP_SYS_NICE`).

#### /proc/<pid>/numa_maps
- Read: per-VMA NUMA mapping summary for process `<pid>`. One line
  per VMA:
  ```
  <hex_addr> <perms> <mapping_type> <file_or_[stack/[heap]]> N0=<bytes> N1=<bytes> ...
  <flags>
  ```
  Where flags include `stack`, `heap`, `huge`, `file=`, `anon`,
  `dirty`, `swapcache`, `mapmax`, `mapcount`, `bsize`, etc.
- Permissions: `0444` if reader has `ptrace` access to the target
  process; otherwise `-EPERM` (root or same-uid with
  `yama.ptrace_scope=0` typically).
- Units: bytes for `N0=`, `N1=`, …; pages for `mapcount`, etc.
- Quirks: read is O(N) over the target's VMAs and can be expensive
  for large processes.

### Memory overcommit

#### /proc/sys/vm/overcommit_memory
- Read: integer `0`, `1`, or `2`.
  - `0` — heuristic overcommit (default).
  - `1` — always overcommit.
  - `2` — never overcommit; commit limit is `swap + (ram *
    overcommit_ratio / 100)` (or `swap + overcommit_kbytes` if
    non-zero).
- Write: integer `0..2`. Writing `2` synchronously recomputes the
  per-CPU commit batch and the address-space accounting.
- Permissions: `0644`.
- Quirks: changing to `OVERCOMMIT_NEVER` while overcommit is high
  can cause existing `mmap`s to fail on fork — handle with care.

#### /proc/sys/vm/overcommit_ratio
- Read: integer percent `0..100`, default `50`.
- Write: integer percent. Only used when `overcommit_memory == 2`
  and `overcommit_kbytes == 0`.
- Permissions: `0644`.
- Units: percent.

#### /proc/sys/vm/overcommit_kbytes
- Read: integer kbytes, default `0`.
- Write: integer kbytes. If non-zero, takes precedence over
  `overcommit_ratio`; writing a non-zero value zeroes
  `overcommit_ratio`.
- Permissions: `0644`.
- Units: **kB**.

#### /proc/sys/vm/user_reserve_kbytes
- Read: kbytes reserved for root-owned user processes under
  overcommit-never. Default `min(3% of free, 128 MiB)`.
- Write: unsigned long kbytes.
- Permissions: `0644`.
- Units: kB.

#### /proc/sys/vm/admin_reserve_kbytes
- Read: kbytes reserved for root logins under
  overcommit-never. Default `8 MiB`.
- Write: unsigned long kbytes.
- Permissions: `0644`.
- Units: kB.

### OOM killer

#### /proc/sys/vm/panic_on_oom
- Read: integer `0`, `1`, or `2`.
  - `0` — do not panic on OOM (default); invoke OOM killer.
  - `1` — panic if OOM killer cannot find a victim (system-wide).
  - `2` — always panic on OOM.
- Write: integer `0..2`.
- Permissions: `0644`.

#### /proc/sys/vm/oom_kill_allocating_task
- Read: `0` or `1`. When `1`, the OOM killer kills the task that
  triggered the OOM (instead of the highest-oom_score task).
- Write: `0` or `1`.
- Permissions: `0644`.
- Quirks: only takes effect for system-wide OOM; cgroup OOM
  ignores it.

#### /proc/sys/vm/oom_dump_tasks
- Read: `0` or `1`. When `1`, the kernel dumps a task list
  (pid/uid/prio/vm/oom_score) on every OOM event.
- Write: `0` or `1`.
- Permissions: `0644`.

### Swappiness

#### /proc/sys/vm/swappiness
- Read: integer `0..200`, default `60`.
- Write: integer `0..200`. `0` = prefer swapping anonymous pages
  only as last resort; `100` = equal preference for anonymous vs
  file-backed pages; `>100` = prefer swapping anonymous pages.
- Permissions: `0644`.
- Units: percent (relative weight, not a strict percentage).
- Quirks: range was `0..100` historically; expanded to `0..200` in
  v5.18 (May 2022) to allow the VM to prefer swapping anonymous
  pages over reclaiming file-backed pages.

### VFS cache pressure

#### /proc/sys/vm/vfs_cache_pressure
- Read: integer `>= 0`, default `100`.
- Write: integer. `0` never reclaims dentries/inodes; `100` is the
  fair reclaim rate; `>100` increases reclaim pressure on
  dentry/inode caches.
- Permissions: `0644`.
- Units: percent.

#### /proc/sys/vm/vfs_cache_pressure_denom
- Read: integer denominator (advanced internal knob). Default `1`.
- Permissions: `0644`.

### Zone reclaim

#### /proc/sys/vm/zone_reclaim_mode
- Read: integer bitmask, default `0` (disabled).
  - `0` — disable zone reclaim (default on most x86 servers).
  - `1` — enable zone reclaim.
  - `2` — also write dirty file-backed pages out rather than
    discarding clean ones.
  - `4` — also swap anonymous pages.
- Write: integer (typically `0`, `1`, `3`, `5`, `7`).
- Permissions: `0644`.
- Quirks: only present when `CONFIG_NUMA=y`. When non-zero, the
  kernel attempts to reclaim pages from the local Node before
  falling back to remote-node allocation. Recommended only for
  systems with very high remote-access latency (e.g. some HPC
  topologies); can hurt throughput on most servers.

#### /proc/sys/vm/min_unmapped_ratio
- Read: integer percent `0..100`, default `1`. The minimum percent
  of unmapped file-backed pages in a zone required for zone reclaim
  to run.
- Write: integer percent.
- Permissions: `0644`.
- Quirks: only present when `CONFIG_NUMA=y`.

#### /proc/sys/vm/min_slab_ratio
- Read: integer percent `0..100`, default `5`. The minimum percent
  of slab-reclaimable pages required for zone reclaim to run.
- Write: integer percent.
- Permissions: `0644`.
- Quirks: only present when `CONFIG_NUMA=y`.

### Other useful `/proc/sys/vm/` files (read-only context)

The implementation may expose these as read-only introspection
fields. They are not the primary CPU/memory control surface but are
commonly tuned together.

#### /proc/sys/vm/min_free_kbytes
- Read: kbytes of free memory the kernel tries to maintain.
- Write: unsigned int kbytes, range-checked.
- Permissions: `0644`.
- Units: kB.

#### /proc/sys/vm/watermark_scale_factor
- Read: integer percent `1..3000`, default `10`. Scales the gap
  between min/low/high watermarks.
- Write: integer percent.
- Permissions: `0644`.

#### /proc/sys/vm/watermark_boost_factor
- Read: integer, default `15000`. Boosts the high watermark
  proactively when free memory drops.
- Write: integer `>= 0`.
- Permissions: `0644`.

#### /proc/sys/vm/percpu_pagelist_high_fraction
- Read: integer, default `0` (auto). Each per-CPU pagelist can
  hold up to `zone_size / fraction` pages before flushing.
- Write: integer `>= 0`.
- Permissions: `0644`.

#### /proc/sys/vm/lowmem_reserve_ratio
- Read: per-zone ratio array (one int per zone, sysctl-array
  format). Used to compute the reserve that lower zones keep for
  upper-zone allocations.
- Write: same array format.
- Permissions: `0644`.

#### /proc/sys/vm/numa_zonelist_order
- Read: `node` or `zone`. Controls whether zone lists are ordered
  by Node or by zone type.
- Write: `node` or `zone`.
- Permissions: `0644`.
- Quirks: only present when `CONFIG_NUMA=y`. Largely obsolete.

#### /proc/sys/vm/page-cluster
- Read: integer `0..31`, default `3`. Controls how many pages (2^N)
  swap reads in at once.
- Write: integer `0..31`. `0` disables readahead.
- Permissions: `0644`.

#### /proc/sys/vm/stat_interval
- Read: integer seconds, default `1`. How often the VM updates
  per-CPU vm stats.
- Write: integer `>= 0`.
- Permissions: `0644`.

#### /proc/sys/vm/numa_stat
- Read: `0` or `1` — whether to maintain per-node NUMA stats.
- Write: `0` or `1`.
- Permissions: `0644`.
- Quirks: only present when `CONFIG_NUMA=y`.

#### /proc/sys/vm/defrag_mode
- Read: integer `0..1`. v6.16+.
- Permissions: `0644`.

---

## Cross-cutting notes for the implementation

1. **kHz is the universal frequency unit for cpufreq sysfs.** All
   `*_freq` files in `/sys/devices/system/cpu/cpuN/cpufreq/` use
   kHz. The Zenctl public API takes Hz (`int64_t hz` in
   `include/zenctl/cpu.h`); the implementation must multiply by
   `1000` on the way in and divide on the way out. The transition
   latency file is the lone exception — it is in *nanoseconds*.
2. **Boolean writes are inconsistent.** Some files use `kstrtobool`
   (accepts `0/1/n/y/off/on`); others use `kstrtouint` and clamp
   to `0..1`; others use `sscanf("%u")` with manual clamping. The
   implementation should always send the canonical `0` or `1` and
   *not* rely on the more permissive forms.
3. **String writes must match exactly.** `scaling_governor`,
   `energy_performance_preference`, `enabled`, `defrag`,
   `shmem_enabled`, `smt/control`, and `intel_pstate/status` all
   use exact-string matching. Trailing newlines are tolerated
   (most parsers strip them) but leading/trailing spaces are
   not. The implementation should `write(fd, value,
   strlen(value))` without adding whitespace.
4. **Numeric writes accept the integer as a string.** The
   implementation must convert `int64_t`/`uint64_t` to a base-10
   string before writing. Do not send raw binary.
5. **Per-CPU vs. policy-wide effects.** `scaling_governor`,
   `scaling_min_freq`, `scaling_max_freq`, `scaling_setspeed`,
   `energy_performance_preference`, and the per-governor tunables
   all act on the *policy* — writing to CPU 0 may affect CPUs
   0–7 if they share `policy0`. The implementation should read
   `related_cpus` to report affected CPUs, and consider this when
   iterating "set governor on all CPUs" (you only need to write
   once per policy).
6. **`online` is a regular device attribute, not CPU-specific.**
   The same `DEVICE_ATTR_RW(online)` exists on every
   hotpluggable device. The file is created by the driver core,
   not by the CPU subsystem; do not look for it under
   `/sys/devices/system/cpu/cpu0/topology/` or similar.
7. **The `disable` cpuidle file requires `CAP_SYS_ADMIN`.** The
   file is `0644` but the store function returns `-EPERM` without
   the capability. The Zenctl CLI should drop privileges carefully
   or document the requirement.
8. **`/sys/devices/system/cpu/cpuN/cpufreq/` is a symlink.** Open
   it with `openat()` or `realpath()` to avoid races if policies
   are reconfigured while the library is running.
9. **Hugepage writes return the requested count, not the achieved
   count.** `nr_hugepages` may end up lower than requested if
   memory is fragmented; the implementation must re-read the file
   to confirm the actual pool size.
10. **NUMA policy is set via syscalls, not sysfs.** The library
    must use `set_mempolicy(2)` / `mbind(2)` for the equivalent of
    `numactl --membind` / `--interleave`. There is no sysfs
    equivalent.
11. **Some files exist only under specific drivers.**
    `energy_performance_preference` requires intel_pstate or
    amd-pstate. `freqdomain_cpus` requires acpi-cpufreq or
    cppc-cpufreq. `intel_pstate/*` requires intel_pstate. The
    capability probe must check for file existence, not assume it.
12. **Permissions of `cpuinfo_cur_freq` are `0400`, not `0444`.**
    This is the only cpufreq attribute that is root-only-readable.
    The library must be prepared for unprivileged callers to get
    `-EACCES` on this file specifically.
13. **The `microcode/reload` write is picky.** It accepts only the
    literal `1`; any other value (including `0`, `true`, or
    `on`) is rejected. Sending a malformed value will return
    `-EINVAL` without performing any reload.
14. **`ppin` is `0400` (root-only).** Some BIOSes disable PPIN
    read-out at the hardware level; the file may exist but reads
    can fail with `-EACCES` from the MSR access.
15. **SMT `forceoff` is one-way.** Once written, the kernel sets
    `CPU_SMT_FORCE_DISABLED` and subsequent writes return `-EPERM`.
    The implementation should treat `forceoff` as a destructive
    operation requiring user confirmation at the CLI layer.

### Quick reference: file → permission table

| Path | Mode | Notes |
|------|------|-------|
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_driver` | 0444 | string |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_governor` | 0644 | string, must match `scaling_available_governors` |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_available_governors` | 0444 | space-separated |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_min_freq` | 0644 | kHz, integer |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_max_freq` | 0644 | kHz, integer |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_setspeed` | 0644 | kHz, integer; `userspace` gov only |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq` | 0444 | kHz |
| `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_cur_freq` | **0400** | kHz, root-only |
| `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_avg_freq` | 0444 | kHz |
| `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_min_freq` | 0444 | kHz |
| `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq` | 0444 | kHz |
| `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_transition_latency` | 0444 | **nanoseconds** |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_available_frequencies` | 0444 | kHz list |
| `/sys/devices/system/cpu/cpuN/cpufreq/scaling_boost_frequencies` | 0444 | kHz list (boost only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/related_cpus` | 0444 | CPU list |
| `/sys/devices/system/cpu/cpuN/cpufreq/affected_cpus` | 0444 | CPU list (online subset) |
| `/sys/devices/system/cpu/cpuN/cpufreq/bios_limit` | 0444 | kHz |
| `/sys/devices/system/cpu/cpuN/cpufreq/freqdomain_cpus` | 0444 | CPU list (acpi/cppc only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/boost` | 0644 | bool (per-policy, if supported) |
| `/sys/devices/system/cpu/cpuN/cpufreq/energy_performance_preference` | 0644 | string or 0..255 (intel_pstate/amd-pstate) |
| `/sys/devices/system/cpu/cpuN/cpufreq/energy_performance_available_preferences` | 0444 | string list |
| `/sys/devices/system/cpu/cpuN/cpufreq/auto_select` | 0644 | bool (cppc-cpufreq only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/auto_act_window` | 0644 | µs (cppc-cpufreq only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/energy_performance_preference_val` | 0644 | 0..0xFF (cppc-cpufreq only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/perf_limited` | 0644 | bitmask (cppc-cpufreq only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/stats/time_in_state` | 0444 | `<kHz> <10ms-ticks>` |
| `/sys/devices/system/cpu/cpuN/cpufreq/stats/total_trans` | 0444 | count |
| `/sys/devices/system/cpu/cpuN/cpufreq/stats/trans_table` | 0444 | matrix |
| `/sys/devices/system/cpu/cpuN/cpufreq/stats/reset` | 0200 | any value resets |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/rate_limit_us` | 0644 | µs (schedutil) |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/sampling_rate` | 0644 | µs (ondemand, conservative) |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/up_threshold` | 0644 | percent |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/down_threshold` | 0644 | percent (conservative only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/sampling_down_factor` | 0644 | int |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/ignore_nice_load` | 0644 | bool |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/powersave_bias` | 0644 | 0..1000 (ondemand only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/io_is_busy` | 0644 | bool (ondemand only) |
| `/sys/devices/system/cpu/cpuN/cpufreq/<gov>/freq_step` | 0644 | percent (conservative only) |
| `/sys/devices/system/cpu/cpufreq/boost` | 0644 | bool, global |
| `/sys/devices/system/cpu/intel_pstate/status` | 0644 | `off`/`active`/`passive` |
| `/sys/devices/system/cpu/intel_pstate/no_turbo` | 0644 | bool |
| `/sys/devices/system/cpu/intel_pstate/max_perf_pct` | 0644 | percent |
| `/sys/devices/system/cpu/intel_pstate/min_perf_pct` | 0644 | percent |
| `/sys/devices/system/cpu/intel_pstate/turbo_pct` | 0444 | percent |
| `/sys/devices/system/cpu/intel_pstate/num_pstates` | 0444 | count |
| `/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost` | 0644 | bool |
| `/sys/devices/system/cpu/intel_pstate/energy_efficiency` | 0644 | bool (inverted) |
| `/sys/devices/system/cpu/cpuidle/available_governors` | 0444 | string list |
| `/sys/devices/system/cpu/cpuidle/current_driver` | 0444 | string |
| `/sys/devices/system/cpu/cpuidle/current_governor` | 0644 | string |
| `/sys/devices/system/cpu/cpuidle/current_governor_ro` | 0444 | string |
| `/sys/devices/system/cpu/cpuidle/intel_c1_demotion` | 0644 | bool (Intel only) |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/name` | 0444 | string |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/desc` | 0444 | string |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/latency` | 0444 | µs |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/residency` | 0444 | µs |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/power` | 0444 | milliwatts |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/usage` | 0444 | count |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/rejected` | 0444 | count |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/time` | 0444 | µs |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/above` | 0444 | count |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/below` | 0444 | count |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/disable` | 0644 | bool; **requires `CAP_SYS_ADMIN`** |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/default_status` | 0444 | `enabled`/`disabled` |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/s2idle/usage` | 0444 | count |
| `/sys/devices/system/cpu/cpuN/cpuidle/stateM/s2idle/time` | 0444 | µs |
| `/sys/devices/system/cpu/cpuN/online` | 0644 | bool (only on hotpluggable CPUs) |
| `/sys/devices/system/cpu/smt/control` | 0644 | `on`/`off`/`<N>`/`forceoff` |
| `/sys/devices/system/cpu/smt/active` | 0444 | bool |
| `/sys/devices/system/cpu/cpuN/microcode/version` | 0444 | hex `0x%x` |
| `/sys/devices/system/cpu/cpuN/microcode/processor_flags` | 0444 | hex `0x%x` |
| `/sys/devices/system/cpu/microcode/reload` | 0200 | **literal `1` only** |
| `/sys/devices/system/cpu/cpuN/topology/physical_package_id` | 0444 | int |
| `/sys/devices/system/cpu/cpuN/topology/die_id` | 0444 | int |
| `/sys/devices/system/cpu/cpuN/topology/cluster_id` | 0444 | int |
| `/sys/devices/system/cpu/cpuN/topology/core_id` | 0444 | int |
| `/sys/devices/system/cpu/cpuN/topology/ppin` | **0400** | hex `0x%llx`, root-only |
| `/sys/devices/system/cpu/cpuN/topology/thread_siblings` | 0444 | hex bitmask |
| `/sys/devices/system/cpu/cpuN/topology/thread_siblings_list` | 0444 | decimal CPU list |
| `/sys/devices/system/cpu/cpuN/topology/core_siblings` | 0444 | hex bitmask |
| `/sys/devices/system/cpu/cpuN/topology/core_siblings_list` | 0444 | decimal CPU list |
| `/sys/devices/system/cpu/cpuN/topology/core_cpus[_list]`, `package_cpus[_list]`, `die_cpus[_list]`, `cluster_cpus[_list]` | 0444 | new names for above |
| `/sys/devices/system/cpu/cpuN/cpu_capacity` | 0444 | int (ARM/ARM64 only) |
| `/sys/devices/system/cpu/cpuN/power/energy_perf_bias` | 0644 | int 0..15 or string (Intel) |
| `/proc/sys/vm/nr_hugepages` | 0644 | pages (default size only) |
| `/proc/sys/vm/nr_hugepages_mempolicy` | 0644 | pages (NUMA only) |
| `/proc/sys/vm/nr_overcommit_hugepages` | 0644 | pages |
| `/proc/sys/vm/hugetlb_shm_group` | 0644 | gid_t |
| `/proc/sys/vm/movable_gigantic_pages` | 0644 | bool (if enabled) |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/nr_hugepages` | 0644 | pages |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/nr_hugepages_mempolicy` | 0644 | pages (NUMA only) |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/nr_overcommit_hugepages` | 0644 | pages |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/free_hugepages` | 0444 | pages |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/resv_hugepages` | 0444 | pages |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/surplus_hugepages` | 0444 | pages |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/demote_size` | 0644 | `<size>kB` |
| `/sys/kernel/mm/hugepages/hugepages-<size>kB/demote` | 0200 | page count |
| `/sys/devices/system/node/nodeN/hugepages/hugepages-<size>kB/nr_hugepages` | 0644 | pages (per-node) |
| `/sys/devices/system/node/nodeN/hugepages/hugepages-<size>kB/free_hugepages` | 0444 | pages |
| `/sys/devices/system/node/nodeN/hugepages/hugepages-<size>kB/surplus_hugepages` | 0444 | pages |
| `/sys/kernel/mm/transparent_hugepage/enabled` | 0644 | `always`/`madvise`/`never` |
| `/sys/kernel/mm/transparent_hugepage/hugepages-<size>kB/enabled` | 0644 | `always`/`madvise`/`never`/`inherit` |
| `/sys/kernel/mm/transparent_hugepage/defrag` | 0644 | `always`/`defer`/`defer+madvise`/`madvise`/`never` |
| `/sys/kernel/mm/transparent_hugepage/use_zero_page` | 0644 | bool |
| `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size` | 0444 | bytes |
| `/sys/kernel/mm/transparent_hugepage/shrink_underused` | 0644 | bool |
| `/sys/kernel/mm/transparent_hugepage/shmem_enabled` | 0644 | `always`/`within_size`/`advise`/`never`/`deny`/`force` |
| `/sys/kernel/mm/transparent_hugepage/hugepages-<size>kB/shmem_enabled` | 0644 | `always`/`inherit`/`never`/`within_size`/`advise` |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/defrag` | 0644 | bool |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan` | 0644 | int |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed` | 0444 | count |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/full_scans` | 0444 | count |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs` | 0644 | ms |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/alloc_sleep_millisecs` | 0644 | ms |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_none` | 0644 | int |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_swap` | 0644 | int |
| `/sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_shared` | 0644 | int |
| `/sys/devices/system/node/online`, `possible`, `has_normal_memory`, `has_cpu`, `has_high_memory` | 0444 | node lists |
| `/sys/devices/system/node/nodeN/cpumap` | 0444 | hex bitmask |
| `/sys/devices/system/node/nodeN/cpulist` | 0444 | CPU list |
| `/sys/devices/system/node/nodeN/meminfo` | 0444 | kB (HugePages fields in pages!) |
| `/sys/devices/system/node/nodeN/numastat` | 0444 | pages |
| `/sys/devices/system/node/nodeN/distance` | 0444 | int list |
| `/sys/devices/system/node/nodeN/vmstat` | 0444 | pages |
| `/sys/devices/system/node/nodeN/compact` | 0200 | write triggers compaction |
| `/sys/devices/system/node/nodeN/reclaim` | 0200 | write triggers reclaim |
| `/sys/devices/system/node/nodeN/accessY/initiators/{read,write}_{bandwidth,latency}` | 0444 | MB/s or ns |
| `/sys/devices/system/node/nodeN/memory_side_cache/indexY/{indexing,line_size,size,write_policy,address_mode}` | 0444 | various |
| `/sys/devices/system/node/nodeN/memory_failure/{total,ignored,failed,delayed,recovered}` | 0444 | pages |
| `/sys/devices/system/node/nodeN/x86/sgx_total_bytes` | 0444 | bytes |
| `/proc/sys/vm/overcommit_memory` | 0644 | `0`/`1`/`2` |
| `/proc/sys/vm/overcommit_ratio` | 0644 | percent |
| `/proc/sys/vm/overcommit_kbytes` | 0644 | kB |
| `/proc/sys/vm/user_reserve_kbytes` | 0644 | kB |
| `/proc/sys/vm/admin_reserve_kbytes` | 0644 | kB |
| `/proc/sys/vm/panic_on_oom` | 0644 | `0`/`1`/`2` |
| `/proc/sys/vm/oom_kill_allocating_task` | 0644 | bool |
| `/proc/sys/vm/oom_dump_tasks` | 0644 | bool |
| `/proc/sys/vm/swappiness` | 0644 | 0..200 |
| `/proc/sys/vm/vfs_cache_pressure` | 0644 | int (default 100) |
| `/proc/sys/vm/vfs_cache_pressure_denom` | 0644 | int |
| `/proc/sys/vm/zone_reclaim_mode` | 0644 | bitmask (NUMA only) |
| `/proc/sys/vm/min_unmapped_ratio` | 0644 | percent (NUMA only) |
| `/proc/sys/vm/min_slab_ratio` | 0644 | percent (NUMA only) |
| `/proc/sys/vm/min_free_kbytes` | 0644 | kB |
| `/proc/sys/vm/watermark_scale_factor` | 0644 | percent 1..3000 |
| `/proc/sys/vm/watermark_boost_factor` | 0644 | int |
| `/proc/sys/vm/percpu_pagelist_high_fraction` | 0644 | int |
| `/proc/sys/vm/lowmem_reserve_ratio` | 0644 | per-zone int array |
| `/proc/sys/vm/numa_zonelist_order` | 0644 | `node`/`zone` (NUMA only) |
| `/proc/sys/vm/page-cluster` | 0644 | 0..31 |
| `/proc/sys/vm/stat_interval` | 0644 | seconds |
| `/proc/sys/vm/numa_stat` | 0644 | bool (NUMA only) |
| `/proc/sys/vm/defrag_mode` | 0644 | 0..1 |
| `/proc/<pid>/numa_maps` | 0444 | per-VMA NUMA mapping (ptrace-gated) |

### Quick reference: unit table

| Quantity | Unit | Where |
|----------|------|-------|
| cpufreq frequencies (`scaling_*_freq`, `cpuinfo_*_freq`, `scaling_available_frequencies`, `scaling_setspeed`, `bios_limit`, `boost` entries) | **kHz** | `/sys/devices/system/cpu/cpuN/cpufreq/*` |
| cpufreq transition latency | **nanoseconds** | `cpuinfo_transition_latency` |
| cpufreq `time_in_state` time column | 10 ms ticks | `stats/time_in_state` |
| cpuidle exit latency | **microseconds** | `stateM/latency` |
| cpuidle target residency | **microseconds** | `stateM/residency` |
| cpuidle time-in-state | **microseconds** | `stateM/time`, `s2idle/time` |
| cpuidle power | **milliwatts** | `stateM/power` |
| governor `sampling_rate` | **microseconds** | `ondemand/sampling_rate`, `conservative/sampling_rate` |
| governor `rate_limit_us` | **microseconds** | `schedutil/rate_limit_us` |
| governor thresholds, `freq_step`, `*_pct`, `turbo_pct` | percent | various |
| EPP raw value | 0..255 | `energy_performance_preference` (when numeric) |
| EPB | 0..15 | `power/energy_perf_bias` |
| Intel `auto_act_window` | **microseconds** | `cpufreq/auto_act_window` |
| hugepage counts | **pages** | `nr_hugepages`, `free_hugepages`, `surplus_hugepages`, `resv_hugepages` |
| hugepage sizes (sysfs path component) | kB | `hugepages-<size>kB` |
| hugepage `demote_size` | `<size>kB` string | `demote_size` |
| THP `hpage_pmd_size` | **bytes** | `transparent_hugepage/hpage_pmd_size` |
| THP `enabled`/`defrag`/`shmem_enabled` | string | mode keywords |
| khugepaged `*_sleep_millisecs` | **milliseconds** | khugepaged/ |
| khugepaged `pages_to_scan`, `pages_collapsed` | pages | khugepaged/ |
| khugepaged `max_ptes_*` | PTEs (pages, effectively) | khugepaged/ |
| NUMA `distance` | dimensionless (10 = local) | `nodeN/distance` |
| NUMA `meminfo` memory fields | kB | `nodeN/meminfo` |
| NUMA `meminfo` `HugePages_*` fields | **pages** (not kB!) | `nodeN/meminfo` |
| NUMA `numastat`, `vmstat` | pages | `nodeN/numastat`, `nodeN/vmstat` |
| NUMA access bandwidth | MB/s | `accessY/initiators/*_bandwidth` |
| NUMA access latency | nanoseconds | `accessY/initiators/*_latency` |
| NUMA memory-side cache `line_size`, `size` | bytes | `memory_side_cache/` |
| `sgx_total_bytes` | bytes | `nodeN/x86/sgx_total_bytes` |
| `memory_failure/*` | pages | `nodeN/memory_failure/` |
| `numa_maps` `N0=`, `N1=` | bytes | `/proc/<pid>/numa_maps` |
| `overcommit_ratio`, `swappiness`, `watermark_scale_factor`, `vfs_cache_pressure`, `min_unmapped_ratio`, `min_slab_ratio` | percent | `/proc/sys/vm/` |
| `overcommit_kbytes`, `min_free_kbytes`, `user_reserve_kbytes`, `admin_reserve_kbytes` | kB | `/proc/sys/vm/` |
| `page-cluster` | 2^N pages | `/proc/sys/vm/page-cluster` |
| `stat_interval` | seconds | `/proc/sys/vm/stat_interval` |
