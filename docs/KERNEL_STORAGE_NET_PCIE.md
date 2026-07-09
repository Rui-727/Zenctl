# Linux kernel storage, network, and PCIe control interfaces

A precise map of the sysfs, procfs, ioctl, and kernel-parameter
interfaces the Linux kernel exposes for block devices, network
interfaces, and PCIe / IOMMU. This is the spec the Zenctl `lib/storage`,
`lib/net`, and `lib/pcie` implementations code against.

Conventions used below:

- **Path**: exact filesystem path. `<dev>` is a block device name
  (`nvme0n1`, `sda`, `dm-0`...). `<iface>` is a netdev name (`eth0`,
  `enp3s0`, `wlp2s0`...). `<addr>` is a PCI address in
  `DDDD:BB:DD.F` form (e.g. `0000:00:1c.0`). `<N>` is a numeric
  value.
- **Read**: what a `cat` returns. Newline-terminated unless noted.
- **Write**: what an `echo` must send. Quoted strings must match
  exactly (case-insensitive for some files — noted where relevant).
- **Permissions**: octal mode. Sysfs files are owned `root:root` by
  default. `0444` = read-only, `0644` = root-writable, `0200` =
  write-only.
- **Units**: bytes, sectors (512 B), kB, MiB/s, microseconds,
  milliseconds, packets.
- **Quirks**: format gotchas the implementation must obey.

The canonical ABI reference is `Documentation/ABI/testing/sysfs-block*`,
`Documentation/ABI/testing/sysfs-bus-pci`,
`Documentation/ABI/testing/sysfs-class-net*`,
`Documentation/ABI/testing/sysfs-nvme`, `Documentation/block/stat.rst`,
`Documentation/networking/scaling.rst`,
`Documentation/networking/operstates.rst`,
`Documentation/PCI/sysfs-pci.rst`,
`Documentation/core-api/irq/irq-affinity.rst`,
`Documentation/admin-guide/kernel-parameters.txt`, and the source in
`block/blk-sysfs.c`, `block/elevator.c`, `drivers/scsi/scsi_sysfs.c`,
`drivers/scsi/sd.c`, `drivers/nvme/host/sysfs.c`,
`drivers/nvme/host/multipath.c`, `drivers/pci/pci-sysfs.c`,
`drivers/pci/pcie/aspm.c`, `drivers/iommu/iommu.c`,
`drivers/iommu/iommu-sysfs.c`, `net/core/net-sysfs.c`,
`include/uapi/linux/ethtool.h`. This document reflects Linux v6.20-rc1
(post-6.19 master as of writing).

---

## Storage — block layer

### Block sysfs layout

Each block device exposes two kobjects in sysfs:

- `/sys/block/<dev>/` — the `queue` subdirectory lives here, holding
  block-layer generic attributes created by `block/blk-sysfs.c`. These
  are the same for NVMe, SCSI, virtio, loop, dm, md, etc.
- `/sys/block/<dev>/device/` — a *symbolic link* to the underlying
  hardware device. For NVMe namespaces it points to
  `/sys/devices/.../nvme/<ctrl>/<ns>/`. For SCSI disks it points to
  `/sys/devices/.../<host>/<bus>:<target>:<lun>/`. The files in this
  directory are bus/driver-specific (e.g. `queue_depth`,
  `cache_type`, `state`, `model`, `vendor`). For virtual devices
  (loop, dm, md, zram) `device/` does not exist.
- `/sys/block/<dev>/stat` — a single-line 17-field summary, lives
  directly under the block device, *not* in `queue/`.

Two attribute groups are registered in `blk-sysfs.c`:

- `queue_attrs` — applies to *all* queues (bio-based or
  request-based). Listed in `queue_attr_group`.
- `blk_mq_queue_attrs` — applies only to blk-mq (request-based)
  queues. Listed in `blk_mq_queue_attr_group`. Files here return
  mode `0` (invisible) for bio-based queues via
  `blk_mq_queue_attr_visible()`.

`nr_requests`, `async_depth`, `scheduler`, `wbt_lat_usec`,
`rq_affinity`, `io_timeout` are all in the blk-mq-only group.

`queue_attr_visible()` further hides zoned-only files
(`max_open_zones`, `max_active_zones`, `zoned_qd1_writes`) on
non-zoned devices.

The macros defining entries:

- `QUEUE_RO_ENTRY`    → mode `0444`
- `QUEUE_RW_ENTRY`    → mode `0644`
- `QUEUE_LIM_RO_ENTRY` → mode `0444`, takes `q->limits` lock
- `QUEUE_LIM_RW_ENTRY` → mode `0644`, takes `q->limits` lock on both
  show and store

#### /sys/block/<dev>/queue/scheduler
- Read: space-separated list of registered elevator names with the
  active one in square brackets, e.g.
  `[mq-deadline] kyber bfq none\n` or `mq-deadline kyber bfq [none]\n`.
  Built in `block/elevator.c::elv_iosched_show()`. The list iterates
  `elv_list`; the current scheduler is bracketed. `none` is always
  present (the no-op "scheduler"); when no elevator is attached,
  `[none]` is the first entry.
- Write: the name of an elevator to switch to (e.g. `bfq`, `none`).
  Trailing whitespace tolerated. The core does
  `request_module("iosched-%s", name)` first to allow modular
  schedulers, then freezes the queue and calls
  `elevator_switch()`. Unknown names return `-EINVAL`.
- Permissions: `0644`.
- Units: N/A (string).
- Quirks: only visible on blk-mq queues. Switching to `none`
  disables merging and request reordering; the queue runs purely as
  a FIFO. Writing the currently-active name is a no-op. The queue
  must be registered (`blk_queue_registered(q)`) or the write
  returns `-ENOENT`. The default scheduler is set by
  `/sys/module/<scheduler>/parameters/default` and by the
  `elevator=` kernel cmdline param.

#### /sys/block/<dev>/queue/nr_requests
- Read: the per-hardware-queue software tag depth (decimal).
- Write: decimal integer. Silently clamped up to `BLKDEV_MIN_RQ`
  (= 4). Must be `> set->reserved_tags` and `<= MAX_SCHED_RQ`
  (= 32768) when an elevator is attached, or `<= set->queue_depth`
  otherwise. Out-of-range returns `-EINVAL`. Returns `-EBUSY` if
  the queue is in the middle of an HW-queue-count change.
- Permissions: `0644`.
- Units: requests.
- Quirks: blk-mq-only. Changing it allocates/frees scheduler tag
  memory; this can fail with `-ENOMEM` and the value is left
  unchanged. Does not change the hardware submission queue depth
  on NVMe (that's fixed at controller initialisation; see
  `/sys/block/<dev>/device/queue_depth` below).

#### /sys/block/<dev>/queue/read_ahead_kb
- Read: read-ahead size in kB.
- Write: decimal integer in kB.
- Permissions: `0644`.
- Units: kB. Internally stored as `bdi->ra_pages` (pages); the
  show/store conversion is `kb = ra_pages << (PAGE_SHIFT - 10)`.
- Quirks: a write of `0` disables read-ahead. The default for
  rotational disks is `128` kB; for SSDs / NVMe it is usually
  `128` as well but bdi init may set it to `0` on flash. The value
  is a hint — the kernel may exceed it for sequential prefetch and
  ignore it for O_DIRECT / `O_RANDOM`.

#### /sys/block/<dev>/queue/discard_max_bytes
- Read: maximum discard (TRIM) byte count per request that the
  device will accept. Stored as `limits.max_discard_sectors` and
  shown in bytes (× 512).
- Write: decimal integer in bytes. Clamped to
  `<= max_hw_discard_sectors << 9` and aligned down to
  `discard_granularity`.
- Permissions: `0644`.
- Units: bytes.
- Quirks: writing `0` disables discard entirely on the queue. The
  hardware maximum (read-only) is in `discard_max_hw_bytes`.

#### /sys/block/<dev>/queue/discard_max_hw_bytes
- Read: hardware maximum discard byte count per request.
- Write: not allowed.
- Permissions: `0444`.
- Units: bytes.
- Quirks: shown as `max_discard_sectors << 9`. Some devices report
  `0xffffffff` (4 GiB − 512 B) which is the largest value the
  NVMe DSM command accepts.

#### /sys/block/<dev>/queue/discard_granularity
- Read: smallest discardable unit in bytes. Equals the logical
  block size for most SSDs; for SMR / zoned devices equals the
  zone size.
- Write: not allowed.
- Permissions: `0444`.
- Units: bytes.
- Quirks: zero for devices that do not support discard at all.

#### /sys/block/<dev>/queue/discard_zeroes_data
- Read: always `0`. Historically indicated whether discarded
  blocks read back as zeroes; modern kernels no longer trust the
  device and always report `0`. The file is kept for legacy
  userspace.
- Write: not allowed.
- Permissions: `0444`.
- Quirks: defined by `QUEUE_SYSFS_SHOW_CONST(discard_zeroes_data, 0)`.

#### /sys/block/<dev>/queue/rotational
- Read: `1` if the device is rotational, `0` otherwise. NVMe, SCSI
  SSDs, and most modern drivers clear
  `BLK_FEAT_ROTATIONAL` in their `queue_limits`.
- Write: `0` or `1`. Useful to override an incorrect default
  (e.g. a USB-attached SSD that the kernel still treats as
  rotational).
- Permissions: `0644`.
- Units: boolean.
- Quirks: affects the I/O scheduler's cost model and the read-ahead
  heuristic; switching to `0` on a real rotational disk hurts.

#### /sys/block/<dev>/queue/write_cache
- Read: `write back\n` if the volatile write cache is enabled,
  `write through\n` if disabled. Built in
  `blk-sysfs.c::queue_wc_show()` based on `BLK_FEAT_WRITE_CACHE`.
- Write: one of `write back`, `write through`, or `none`. The
  latter two are equivalent (both set
  `BLK_FLAG_WRITE_CACHE_DISABLED` in the new `queue_limits`).
  Compared with `strncmp`, so trailing whitespace is tolerated.
  Invalid input returns `-EINVAL`.
- Permissions: `0644`.
- Units: N/A (string).
- Quirks: requires the device to actually advertise a volatile
  write cache. For NVMe this is mapped to the controller's
  `VWC` field; changing it issues
  `Set Features - Volatile Write Cache`. For SCSI it issues
  `MODE SELECT` to switch the WCE bit. The `fua` file
  (read-only) indicates whether the device supports
  Forced Unit Access, the preferred way to flush without
  disabling the cache entirely.

#### /sys/block/<dev>/queue/wbt_lat_usec
- Read: the writeback-throttling target latency in microseconds,
  or `0` if WBT is disabled, or `-EINVAL` if WBT is not loaded
  (`CONFIG_BLK_WBT=n`).
- Write: decimal integer in microseconds. The value `-1` is
  accepted and means "use the default" (which is `7500` usec for
  SSDs, `75000` usec for rotational disks). Values `< -1` return
  `-EINVAL`. Writing `0` disables WBT.
- Permissions: `0644`.
- Units: microseconds. Internally stored in nanoseconds and shown
  via `div_u64(wbt_get_min_lat(q), 1000)`.
- Quirks: blk-mq-only. Requires `CONFIG_BLK_WBT`. WBT is
  auto-disabled when an elevator is loaded for some schedulers
  (e.g. BFQ) and re-enabled when switching back. When `wbt`
  is disabled the read returns `0`.

#### /sys/block/<dev>/queue/rq_affinity
- Read: bitmap: `1` = same-CPU completion preferred, `2` =
  same-CPU-group completion preferred.
- Write: `0`, `1`, `2`, or `3`.
- Permissions: `0644`.
- Units: bitmap.
- Quirks: blk-mq-only.

#### /sys/block/<dev>/queue/io_poll
- Read: `1` if hybrid polling is enabled on this queue, `0`
  otherwise.
- Write: `0` or `1`.
- Permissions: `0644`.
- Quirks: requires the driver to actually support polling
  (the `mq_ops->poll` callback must be set); otherwise writes
  return `-EINVAL`.

#### /sys/block/<dev>/queue/io_timeout
- Read: the request timeout in milliseconds.
- Write: positive decimal integer in milliseconds. `0` returns
  `-EINVAL`.
- Permissions: `0644`.
- Units: milliseconds. Converted internally to jiffies.
- Quirks: blk-mq-only. Hidden when the driver does not implement
  `mq_ops->timeout`.

#### /sys/block/<dev>/queue/nomerges
- Read: `0` (allow all merges), `1` (only one-shot merges),
  `2` (no merges at all).
- Write: `0`, `1`, or `2`.
- Permissions: `0644`.

#### /sys/block/<dev>/queue/logical_block_size
- Read: the smallest addressable unit in bytes (typically
  `512`, `4096`, or `520` for CD/DVD). Sent in `READ/WRITE`
  commands; the FS block size must be a multiple of this.
- Write: not allowed.
- Permissions: `0444`.
- Units: bytes.

#### /sys/block/<dev>/queue/physical_block_size
- Read: the physical block size in bytes (the medium's actual
  erase/write unit). Always `>=` the logical block size.
- Write: not allowed.
- Permissions: `0444`.
- Units: bytes.

#### /sys/block/<dev>/queue/max_sectors_kb
- Read: the maximum number of kilobytes per single I/O request
  the block layer will build. Driver-clamped.
- Write: decimal integer in kB. Clamped to
  `<= max_hw_sectors_kb` and `>=` the logical block size.
- Permissions: `0644`.
- Units: kB.

#### /sys/block/<dev>/queue/max_hw_sectors_kb
- Read: hardware maximum request size in kB.
- Write: not allowed.
- Permissions: `0444`.
- Units: kB.

#### /sys/block/<dev>/queue/io_min, io_opt
- Read: minimum / optimal I/O size in bytes reported by the
  device. `io_opt` is the stride that gives the best
  performance (e.g. the RAID stripe size).
- Write: not allowed.
- Permissions: `0444`.
- Units: bytes.

#### /sys/block/<dev>/queue/max_segments, max_segment_size
- Read: maximum number of physical segments per request, and
  the maximum size of a single segment in bytes.
- Write: not allowed.
- Permissions: `0444`.
- Units: count, bytes.

### /sys/block/<dev>/stat — the 17 fields

Documented in `Documentation/block/stat.rst`. Single line, 17
whitespace-separated decimal values. A single file guarantees a
consistent snapshot.

```
read_ios  read_merges  read_sectors  read_ticks
write_ios write_merges write_sectors write_ticks
in_flight io_ticks time_in_queue
discard_ios discard_merges discard_sectors discard_ticks
flush_ios flush_ticks
```

| # | field            | units         | description                                  |
|---|------------------|---------------|----------------------------------------------|
| 1 | read I/Os        | requests      | completed read requests                      |
| 2 | read merges      | requests      | reads merged in-queue                        |
| 3 | read sectors     | sectors (512B)| sectors read                                 |
| 4 | read ticks       | ms            | total wait time for reads                    |
| 5 | write I/Os       | requests      | completed write requests                     |
| 6 | write merges     | requests      | writes merged in-queue                       |
| 7 | write sectors    | sectors (512B)| sectors written                              |
| 8 | write ticks      | ms            | total wait time for writes                   |
| 9 | in_flight        | requests      | I/Os currently issued but not complete       |
| 10| io_ticks         | ms            | total time device had I/O queued             |
| 11| time_in_queue    | ms            | sum of (waiters × ms) — weighted queue depth |
| 12| discard I/Os     | requests      | completed discards                           |
| 13| discard merges   | requests      | discards merged in-queue                     |
| 14| discard sectors  | sectors (512B)| sectors discarded                            |
| 15| discard ticks    | ms            | total wait time for discards                 |
| 16| flush I/Os       | requests      | completed flush requests (not tracked for    |
|   |                  |               | partitions)                                  |
| 17| flush ticks      | ms            | total wait time for flushes                  |

- Permissions: `0444`.
- Units: see table. "Sectors" are standard 512-byte UNIX sectors
  regardless of the device's logical block size.
- Quirks: `read_ticks` / `write_ticks` / `discard_ticks` /
  `flush_ticks` count *total wait time* across all requests; if
  N requests wait simultaneously the counter advances at N× the
  real-time rate. `time_in_queue` is the integral of queue depth
  over time, also called the "weighted # of requests waiting".
  Partitions expose their own `stat` files but `flush_ios` /
  `flush_ticks` are always 0 for partitions. The partition file
  is `/sys/block/<dev>/<part>/stat` (e.g.
  `/sys/block/sda/sda1/stat`).

### Per-device (bus-specific) attributes

These live in `/sys/block/<dev>/device/` (a symlink to the parent
hardware device) and are created by the bus / LLD.

#### /sys/block/<dev>/device/queue_depth  (SCSI, NVMe multipath)
- Read: for SCSI disks, `sdev->queue_depth` — the number of
  commands the host can issue to the LUN at once. For NVMe this
  file exists only when the namespace is part of a multipath
  controller using the `queue-depth` I/O policy
  (`NVME_IOPOLICY_QD`); otherwise the file is absent and the
  hardware queue depth is fixed by `nvme.io_queue_depth` module
  param (default `1024`, range `[2, 4095]`).
- Write (SCSI only): positive decimal integer. Must be `>= 1`
  and `<= sdev->host->can_queue`. Calls
  `sht->change_queue_depth()`; if the LLD does not implement
  that callback the write returns `-EINVAL`. After a successful
  write `sdev->max_queue_depth` is updated.
- Permissions: SCSI `0644` (`S_IRUGO | S_IWUSR`), NVMe `0444`.
- Units: commands.
- Quirks: for NVMe the per-controller `io_queue_depth` module
  param is at `/sys/module/nvme/parameters/io_queue_depth` and
  is writable at runtime (subject to `>= 2` and `< 4096`).

#### /sys/block/<dev>/device/cache_type  (SCSI `sd` only)
- Read: one of `write through`, `none`, `write back`,
  `write back, no read (daft)`. Defined in `drivers/scsi/sd.c`:
  `static const char *sd_cache_types[]` indexed by the WCE/RCD
  bits of the MODE SELECT page 0x08.
- Write: one of the four strings above. Compared with
  `sysfs_match_string()` — case-sensitive, no trailing
  whitespace. Invalid input returns `-EINVAL`.
- Permissions: `0644`.
- Units: N/A (string).
- Quirks: SCSI-only — does not exist for NVMe. For NVMe the
  write cache is controlled via `queue/write_cache` (above).
  `none` and `write through` both disable the volatile write
  cache (`WCE=0`); they differ only in the RCD bit which is
  rarely meaningful. `write back, no read (daft)` is the broken
  mode where WCE=1 RCD=1.

#### /sys/block/<dev>/device/timeout  (SCSI)
- Read: the request timeout in seconds
  (`sdev->request_queue->rq_timeout / HZ`).
- Write: positive decimal integer in seconds. `<= 0` returns
  `-EINVAL`.
- Permissions: `0644`.
- Units: seconds.

#### /sys/block/<dev>/device/eh_timeout  (SCSI)
- Read: the error-handler timeout in seconds.
- Write: positive decimal integer. Requires `CAP_SYS_ADMIN`.
- Permissions: `0644`.

#### /sys/block/<dev>/device/state  (SCSI)
- Read: current SCSI device state: `running`, `offline`,
  `blocked`, `cancelled`, `quiescent`.
- Write: one of `running`, `offline`, `blocked`. Used to attach
  / detach a LUN without removing it.
- Permissions: `0644`.

#### /sys/block/<dev>/device/queue_ramp_up_period  (SCSI)
- Read: the time over which the kernel gradually raises
  `queue_depth` after a device reset, in milliseconds.
- Write: positive decimal integer.
- Permissions: `0644`.
- Units: milliseconds.

### /sys/class/nvme/nvmeN/ — NVMe controller class

The `nvme` class lives at `/sys/class/nvme/`. Each entry
`nvmeN` is a symlink to `/sys/devices/.../nvme/nvmeN` (the
controller device, not a namespace). Namespaces appear at
`/sys/class/block/nvmeNnM` and link to
`/sys/devices/.../nvme/<ctrl>/<ns>/`.

`nvme_dev_attrs` (built in `drivers/nvme/host/sysfs.c`) defines
the following controller attributes:

| file                  | mode  | read                                                    |
|-----------------------|-------|---------------------------------------------------------|
| `reset_controller`    | 0200  | write `1` to reset                                      |
| `rescan_controller`   | 0200  | write `1` to rescan namespaces                          |
| `model`               | 0444  | controller model string                                 |
| `serial`              | 0444  | serial number                                           |
| `firmware_rev`        | 0444  | active firmware revision                                |
| `cntlid`              | 0444  | controller ID (hex)                                     |
| `delete_controller`   | 0200  | write `1` to remove (fabrics only)                      |
| `transport`           | 0444  | `pcie`, `tcp`, `fc`, `rdma`, `loop`                     |
| `subsysnqn`           | 0444  | subsystem NQN                                           |
| `address`             | 0444  | transport-specific address (PCI BDF for `pcie`)         |
| `state`               | 0444  | `new`, `live`, `resetting`, `connecting`, `deleting`,   |
|                       |       | `deleting`→`dead`                                       |
| `numa_node`           | 0444  | NUMA node (same value as PCI `numa_node`)               |
| `queue_count`         | 0444  | number of queues including admin                        |
| `sqsize`              | 0444  | submission queue size minus one                         |
| `hostnqn`, `hostid`   | 0444  | host NQN / UUID (fabrics only)                          |
| `ctrl_loss_tmo`       | 0644  | controller-loss timeout (seconds) — fabrics             |
| `reconnect_delay`     | 0644  | reconnect delay (seconds) — fabrics                     |
| `fast_io_fail_tmo`    | 0644  | fast-I/O-fail timeout (seconds) — fabrics               |
| `kato`                | 0644  | keep-alive timeout (seconds)                            |
| `cntrltype`           | 0444  | `io`, `discovery`, `admin`                              |
| `dctype`              | 0444  | discovery-controller type                               |
| `quirks`              | 0444  | bitmap of known quirks (hex)                            |
| `admin_timeout`       | 0644  | admin command timeout (seconds)                         |
| `io_timeout`          | 0644  | I/O command timeout (seconds)                           |
| `passthru_err_log_enabled` | 0644 | admin / io passthrough error logging toggle         |
| `tls_key`             | 0444  | NVMe-TLS PSK id (hex)                                   |
| `tls_configured_key`  | 0444  | configured PSK serial                                   |
| `dhchap_secret`       | 0644  | DH-HMAC-CHAP host secret (write-only when set)          |
| `dhchap_ctrl_secret`  | 0644  | DH-HMAC-CHAP controller secret                          |

The per-namespace directory `/sys/block/nvmeNnM/` contains:

- `queue/` — generic block queue (see above).
- `device/` — symlink to the namespace device under
  `/sys/devices/.../nvme/<ctrl>/<ns>/`. Namespace attributes
  include `wwid`, `uuid`, `nguid`, `eui`, `nsid`, `csi`,
  `queue_depth` (multipath QD policy only), `numa_nodes`
  (multipath), `ana_state` (multipath), `multipath_failover_count`,
  `command_error_count`, `command_retries_count`.

### Block-device power management

`/sys/block/<dev>/device/power/` is the standard PM core
directory created by `drivers/base/power/sysfs.c` for any
`struct device`. Applies to NVMe controllers, SCSI LUNs, etc.

#### /sys/block/<dev>/device/power/control
- Read: `on` or `auto`.
- Write: `on` (disable runtime PM for this device) or `auto`
  (allow the PM core to suspend it when idle).
- Permissions: `0644`.
- Quirks: writing `on` calls `pm_runtime_forbid()` which
  increments the device's `power.usage_count`. Setting `auto`
  decrements it. This affects whether the device can be put
  into runtime D3.

#### /sys/block/<dev>/device/power/autosuspend_delay_ms
- Read: the autosuspend delay in milliseconds, `-1` means
  autosuspend is disabled.
- Write: decimal integer in milliseconds. `0` is allowed
  (suspend as soon as idle). Negative values are rejected.
- Permissions: `0644`.
- Units: milliseconds.
- Quirks: only meaningful when `control` = `auto`.

#### /sys/block/<dev>/device/power/runtime_status
- Read: one of `unsupported`, `active`, `suspended`,
  `suspending`, `resuming`.
- Write: not allowed.
- Permissions: `0444`.

#### /sys/block/<dev>/device/power/wakeup
- Read: `enabled` or `disabled` (whether the device can wake
  the system).
- Write: `enabled` or `disabled`.
- Permissions: `0644`.
- Quirks: NVMe over PCIe requires `nvme.controller` to support
  APST and the platform must have wired the PME pin; for many
  consumer NVMe SSDs wake-from-D3 is unreliable.

#### /sys/block/<dev>/device/power/control for NVMe
- The NVMe PCIe driver registers a runtime-PM callback that
  puts the controller into D3 when the namespace is idle.
  Combined with APST (Autonomous Power State Transition) this
  is the main power lever for laptop NVMe SSDs. APST itself
  is configured at controller init via the
  `nvme.apst` module param (default `1`) and the per-state
  latencies are read from the controller's `APSTA` field.

---

## Network — sysfs

### /sys/class/net/<iface>/ layout

Every netdev (physical, virtual, slave) gets a directory here.
Symlink to `/sys/devices/.../net/<iface>`. The file list comes
from `net_class_attrs[]` in `net/core/net-sysfs.c`. Files below
are present on every interface unless noted.

#### /sys/class/net/<iface>/address
- Read: hardware (MAC) address in colon-separated hex, e.g.
  `00:11:22:33:44:55\n`. For non-Ethernet devices the length
  varies; `addr_len` gives the byte count.
- Write: not allowed (use `ip link set address` / RTM_SETLINK).
- Permissions: `0444`.

#### /sys/class/net/<iface>/broadcast
- Read: broadcast address, e.g. `ff:ff:ff:ff:ff:ff\n`.
- Permissions: `0444`.

#### /sys/class/net/<iface>/speed
- Read: link speed in Mbit/s. Returns `-EINVAL` if the device
  is not running, the driver doesn't implement
  `ethtool_ops->get_link_ksettings`, or the link is down (in
  which case the ethtool layer reports `SPEED_UNKNOWN` =
  `-1` and `sysfs_emit` prints that).
- Write: not allowed.
- Permissions: `0444`.
- Units: Mbit/s.
- Quirks: only meaningful for interfaces that implement
  `get_link_ksettings` — mostly Ethernet. WIFI, loopback, and
  tunnels return `-EINVAL`.

#### /sys/class/net/<iface>/duplex
- Read: `half`, `full`, or `unknown`.
- Write: not allowed.
- Permissions: `0444`.
- Quirks: same visibility rules as `speed`.

#### /sys/class/net/<iface>/mtu
- Read: current MTU in bytes.
- Write: positive decimal integer. The driver's
  `ndo_change_mtu` is called via `dev_set_mtu()`. Out-of-range
  values return `-EINVAL`.
- Permissions: `0644`.
- Units: bytes.
- Quirks: the device must be `up` for some drivers to accept
  MTU changes. Default for Ethernet is `1500`.

#### /sys/class/net/<iface>/carrier
- Read: `1` if `netif_carrier_ok(dev)` is true (L1 up),
  `0` otherwise. Returns `-EINVAL` if the device is down.
- Write: `0` or `1` for soft devices (bond, team, dsa) that
  implement `ndo_change_carrier`; `-EOPNOTSUPP` for hardware
  NICs.
- Permissions: `0644`.
- Quirks: the read calls `linkwatch_sync_dev()` first so the
  value reflects the latest L1 event.

#### /sys/class/net/<iface>/operstate
- Read: RFC 2863 operational state as a string:
  `unknown`, `notpresent`, `down`, `lowerlayerdown`,
  `testing`, `dormant`, `up`. Mapped from `dev->operstate`
  via the `operstates[]` table in `net-sysfs.c`.
- Write: not allowed (use RTM_SETLINK).
- Permissions: `0444`.
- Quirks: if the interface is administratively down the read
  returns `down` regardless of `dev->operstate`.

#### /sys/class/net/<iface>/tx_queue_len
- Read: the per-netdev `tx_queue_len` (default `1000` for
  Ethernet, `100` for some others).
- Write: non-negative decimal integer. Calls
  `dev_change_tx_queue_len()`.
- Permissions: `0644`.
- Units: packets.

#### /sys/class/net/<iface>/flags
- Read: `dev->flags` as a hex bitmask (`IFF_UP`, `IFF_BROADCAST`,
  `IFF_LOOPBACK`, `IFF_RUNNING`, etc.; see
  `include/uapi/linux/if.h`).
- Write: hex bitmask; OR-ed into `dev->flags` via
  `dev_change_flags()`.
- Permissions: `0644`.

#### /sys/class/net/<iface>/ifindex, iflink
- Read: kernel-wide unique interface index (decimal). `iflink`
  is the ifindex of the parent device; for physical interfaces
  they are equal.
- Permissions: `0444`.

#### /sys/class/net/<iface>/type
- Read: ARP hardware type from `include/uapi/linux/if_arp.h`
  (`1` = Ethernet, `772` = loopback, `801` = ieee80211, ...).
- Permissions: `0444`.

#### /sys/class/net/<iface>/statistics/
- Read-only directory. Each file is a single `u64` decimal
  counter, populated from `struct rtnl_link_stats64`. Defined
  in `include/uapi/linux/if_link.h`. Files include:
  `rx_packets`, `tx_packets`, `rx_bytes`, `tx_bytes`,
  `rx_errors`, `tx_errors`, `rx_dropped`, `tx_dropped`,
  `multicast`, `collisions`, `rx_length_errors`,
  `rx_over_errors`, `rx_crc_errors`, `rx_frame_errors`,
  `rx_fifo_errors`, `rx_missed_errors`, `tx_aborted_errors`,
  `tx_carrier_errors`, `tx_fifo_errors`, `tx_heartbeat_errors`,
  `tx_window_errors`, `rx_compressed`, `tx_compressed`,
  `rx_nohandler`.
- Permissions: `0444`.
- Quirks: reading each file triggers a full
  `ndo_get_stats64()` call, so iterating all files is
  expensive. Use netlink `RTM_GETSTATS` for a single batched
  read.

### IRQ affinity

#### /proc/irq/default_smp_affinity
- Read: default CPU mask applied to newly-allocated IRQs,
  as a hex bitmask string.
- Write: hex bitmask.
- Permissions: `0644`.
- Quirks: default `0xffffffff` (all CPUs).

#### /proc/irq/<N>/smp_affinity
- Read: CPU bitmask (hex, comma-separated 32-bit groups for
  systems with > 32 CPUs, e.g. `00000000,0000000f`).
- Write: hex bitmask. The mask may not be empty; writes that
  zero every bit return `-EINVAL`. If the IRQ chip does not
  support affinity the value is left unchanged.
- Permissions: `0644`.
- Units: hex bitmask, one bit per logical CPU.

#### /proc/irq/<N>/smp_affinity_list
- Read: CPU list in `cpulist` form, e.g. `0-3,8,10-12`.
- Write: cpulist as accepted by `cpulist_parse()`.
- Permissions: `0644`.
- Quirks: easier to script than the bitmask form for systems
  with sparse CPU numbering.

### RPS, RFS, XPS

Documented in `Documentation/networking/scaling.rst`. All files
live under `/sys/class/net/<iface>/queues/`.

#### /sys/class/net/<iface>/queues/rx-<n>/rps_cpus
- Read: CPU bitmask (hex, comma-separated) of CPUs that may
  process receive packets from this RX queue.
- Write: hex bitmask. Empty mask disables RPS on the queue.
- Permissions: `0644`.
- Quirks: requires `CONFIG_RPS`. Ignored if the NIC supports
  RSS (hardware queue steering); RPS is a software fallback
  for single-queue NICs.

#### /sys/class/net/<iface>/queues/rx-<n>/rps_flow_cnt
- Read: number of entries in the per-queue RFS flow table.
- Write: positive decimal integer.
- Permissions: `0644`.
- Quirks: requires `CONFIG_RPS`. The global
  `/proc/sys/net/core/rps_sock_flow_entries` controls the
  socket-side table; the per-queue count should be set to
  `rps_sock_flow_entries / N` where N is the number of RX
  queues.

#### /sys/class/net/<iface>/queues/tx-<n>/xps_cpus
- Read: CPU bitmask of CPUs whose transmissions should be
  steered to this TX queue.
- Write: hex bitmask. Requires `CAP_NET_ADMIN`.
- Permissions: `0644`.
- Quirks: requires `CONFIG_XPS`. Only meaningful for
  multiqueue devices. The store path calls
  `netif_set_xps_queue()`.

#### /sys/class/net/<iface>/queues/tx-<n>/xps_rxqs
- Read: RX-queue bitmap for RX-queue-based XPS (newer API).
- Write: bitmap. Requires `CAP_NET_ADMIN`.
- Permissions: `0644`.

### ethtool ioctl interface

The ethtool ioctl interface is the legacy (now superseded by
ethtool-netlink) mechanism for NIC configuration. It is invoked
via the `SIOCETHTOOL` ioctl on an `AF_INET` socket (or `AF_LOCAL`
on newer kernels). The wire format is:

```c
struct ifreq {
    char  ifr_name[IFNAMSIZ];          /* interface name */
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short           ifru_flags;
        int             ifru_ivalue;
        int             ifru_mtu;
        void           *ifru_data;     /* <-- ethtool cmd pointer */
        /* ... */
    } ifr_ifru;
};
```

For ethtool, `ifr_ifru.ifru_data` points at an
`struct ethtool_cmd` (or one of the other ethtool command
structures). The first `__u32` of every ethtool structure is
the command number (`cmd` field) that selects the operation.

```c
struct ethtool_value {
    __u32 cmd;
    __u32 data;
};
```

Simple on/off ethtool commands take `struct ethtool_value` and
return `0` / `1` in `data`. Complex commands take their own
structure (see below).

Defined in `include/uapi/linux/ethtool.h`:

| command              | value     | payload struct              | purpose                              |
|----------------------|-----------|-----------------------------|--------------------------------------|
| `ETHTOOL_GSET`       | 0x01      | `ethtool_cmd`               | Get link settings (deprecated)       |
| `ETHTOOL_SSET`       | 0x02      | `ethtool_cmd`               | Set link settings (deprecated)       |
| `ETHTOOL_GDRVINFO`   | 0x03      | `ethtool_drvinfo`           | Get driver / firmware / bus info     |
| `ETHTOOL_GREGS`      | 0x04      | `ethtool_regs` + `data[]`   | Get NIC register dump                |
| `ETHTOOL_GWOL`       | 0x05      | `ethtool_wolinfo`           | Get wake-on-LAN settings             |
| `ETHTOOL_SWOL`       | 0x06      | `ethtool_wolinfo`           | Set wake-on-LAN settings             |
| `ETHTOOL_GMSGLVL`    | 0x07      | `ethtool_value`             | Get driver debug level               |
| `ETHTOOL_SMSGLVL`    | 0x08      | `ethtool_value`             | Set driver debug level               |
| `ETHTOOL_NWAY_RST`   | 0x09      | `ethtool_value`             | Restart autonegotiation              |
| `ETHTOOL_GLINK`      | 0x0a      | `ethtool_value`             | Get link-up status (1 = up)          |
| `ETHTOOL_GEEPROM`    | 0x0b      | `ethtool_eeprom` + `data[]` | Read EEPROM                          |
| `ETHTOOL_SEEPROM`    | 0x0c      | `ethtool_eeprom` + `data[]` | Write EEPROM                         |
| `ETHTOOL_GCOALESCE`  | 0x0e      | `ethtool_coalesce`          | Get IRQ coalescing                   |
| `ETHTOOL_SCOALESCE`  | 0x0f      | `ethtool_coalesce`          | Set IRQ coalescing                   |
| `ETHTOOL_GRINGPARAM` | 0x10      | `ethtool_ringparam`         | Get RX/TX ring sizes                 |
| `ETHTOOL_SRINGPARAM` | 0x11      | `ethtool_ringparam`         | Set RX/TX ring sizes                 |
| `ETHTOOL_GPAUSEPARAM`| 0x12      | `ethtool_pauseparam`        | Get pause-frame (flow ctrl) params   |
| `ETHTOOL_SPAUSEPARAM`| 0x13      | `ethtool_pauseparam`        | Set pause-frame params               |
| `ETHTOOL_GRXCSUM`    | 0x14      | `ethtool_value`             | Get RX checksum offload              |
| `ETHTOOL_SRXCSUM`    | 0x15      | `ethtool_value`             | Set RX checksum offload              |
| `ETHTOOL_GTXCSUM`    | 0x16      | `ethtool_value`             | Get TX checksum offload              |
| `ETHTOOL_STXCSUM`    | 0x17      | `ethtool_value`             | Set TX checksum offload              |
| `ETHTOOL_GSG`        | 0x18      | `ethtool_value`             | Get scatter-gather                   |
| `ETHTOOL_SSG`        | 0x19      | `ethtool_value`             | Set scatter-gather                   |
| `ETHTOOL_TEST`       | 0x1a      | `ethtool_test`              | Run self-test                        |
| `ETHTOOL_GSTRINGS`   | 0x1b      | `ethtool_gstrings`          | Get string set (stats names, etc.)   |
| `ETHTOOL_PHYS_ID`    | 0x1c      | `ethtool_value`             | Blink LED for identification         |
| `ETHTOOL_GSTATS`     | 0x1d      | `ethtool_stats` + `u64[]`   | Get NIC-specific statistics          |
| `ETHTOOL_GTSO`       | 0x1e      | `ethtool_value`             | Get TCP Segmentation Offload         |
| `ETHTOOL_STSO`       | 0x1f      | `ethtool_value`             | Set TSO                              |
| `ETHTOOL_GPERMADDR`  | 0x20      | `ethtool_perm_addr` + `[]`  | Get permanent MAC address            |
| `ETHTOOL_GGSO`       | 0x23      | `ethtool_value`             | Get GSO enable                       |
| `ETHTOOL_SGSO`       | 0x24      | `ethtool_value`             | Set GSO enable                       |
| `ETHTOOL_GFLAGS`     | 0x25      | `ethtool_value`             | Get flags bitmap                     |
| `ETHTOOL_SFLAGS`     | 0x26      | `ethtool_value`             | Set flags bitmap                     |
| `ETHTOOL_GPFLAGS`    | 0x27      | `ethtool_value`             | Get driver-private flags             |
| `ETHTOOL_SPFLAGS`    | 0x28      | `ethtool_value`             | Set driver-private flags             |
| `ETHTOOL_GRXFH`      | 0x29      | `ethtool_rxfh`              | Get RX flow hash config              |
| `ETHTOOL_SRXFH`      | 0x2a      | `ethtool_rxfh`              | Set RX flow hash config              |
| `ETHTOOL_GGRO`       | 0x2b      | `ethtool_value`             | Get Generic Receive Offload          |
| `ETHTOOL_SGRO`       | 0x2c      | `ethtool_value`             | Set GRO                              |
| `ETHTOOL_GSSET_INFO` | 0x37      | `ethtool_sset_info`         | Get string-set counts                |
| `ETHTOOL_GRSSH`      | 0x46      | `ethtool_rxfh`              | Get RSS indirection / hash key       |
| `ETHTOOL_SRSSH`      | 0x47      | `ethtool_rxfh`              | Set RSS indirection / hash key       |
| `ETHTOOL_GFEATURES`  | 0x3a      | `ethtool_gfeatures`         | Get device offload settings          |
| `ETHTOOL_SFEATURES`  | 0x3b      | `ethtool_sfeatures`         | Change device offload settings       |
| `ETHTOOL_GCHANNELS`  | 0x3c      | `ethtool_channels`          | Get channel counts                   |
| `ETHTOOL_SCHANNELS`  | 0x3d      | `ethtool_channels`          | Set channel counts                   |
| `ETHTOOL_GET_TS_INFO`| 0x41      | `ethtool_ts_info`           | Get timestamping / PHC info          |
| `ETHTOOL_GMODULEINFO`| 0x42      | `ethtool_modinfo`           | Get SFP/QSFP module info             |
| `ETHTOOL_GMODULEEEPROM`| 0x43    | `ethtool_eeprom`            | Read SFP/QSFP EEPROM                 |
| `ETHTOOL_GEEE`       | 0x44      | `ethtool_eee`               | Get Energy-Efficient Ethernet        |
| `ETHTOOL_SEEE`       | 0x45      | `ethtool_eee`               | Set EEE                              |
| `ETHTOOL_GLINKSETTINGS`| 0x4c    | `ethtool_link_settings`     | Get link settings (modern)           |
| `ETHTOOL_SLINKSETTINGS`| 0x4d    | `ethtool_link_settings`     | Set link settings (modern)           |
| `ETHTOOL_GFECPARAM`  | 0x50      | `ethtool_fecparam`          | Get FEC settings                     |
| `ETHTOOL_SFECPARAM`  | 0x51      | `ethtool_fecparam`          | Set FEC settings                     |
| `ETHTOOL_PERQUEUE`   | 0x4b      | `ethtool_per_queue_attr`    | Per-queue attribute set              |

#### struct ethtool_cmd  (deprecated, use ethtool_link_settings)
- `cmd` (`__u32`): `ETHTOOL_GSET` / `ETHTOOL_SSET`.
- `supported` (`__u32`): bitmask of supported link modes /
  ports / features (read-only).
- `advertising` (`__u32`): link modes currently advertised.
- `speed` (`__u16`) + `speed_hi` (`__u16`): link speed in
  Mbit/s, split low/high. Use `ethtool_cmd_speed()` /
  `_set()` helpers. `SPEED_UNKNOWN = -1` (UINT32_MAX) when
  not negotiated.
- `duplex` (`__u8`): `DUPLEX_HALF` (0), `DUPLEX_FULL` (1),
  `DUPLEX_UNKNOWN` (255).
- `port` (`__u8`): `PORT_TP`, `PORT_AUI`, `PORT_MII`,
  `PORT_FIBRE`, `PORT_BNC`, etc.
- `phy_address` (`__u8`): MDIO address of the PHY.
- `transceiver` (`__u8`): deprecated.
- `autoneg` (`__u8`): `AUTONEG_DISABLE` (0) or
  `AUTONEG_ENABLE` (1).
- `mdio_support` (`__u8`): `ETH_MDIO_SUPPORTS_C22` and/or
  `ETH_MDIO_SUPPORTS_C45`.
- `maxtxpkt` / `maxrxpkt` (`__u32`): deprecated, use
  `ethtool_coalesce`.
- `eth_tp_mdix` (`__u8`): MDI/MDI-X status.
- `eth_tp_mdix_ctrl` (`__u8`): MDI/MDI-X control.
- `lp_advertising` (`__u32`): link-partner advertised modes.
- `reserved[2]`.

#### struct ethtool_link_settings  (modern replacement)
- `cmd` = `ETHTOOL_GLINKSETTINGS` / `ETHTOOL_SLINKSETTINGS`.
- `speed` (`__u32`): Mbps, `SPEED_UNKNOWN` if not negotiated.
- `duplex`, `port`, `phy_address`, `autoneg`, `mdio_support`,
  `eth_tp_mdix`, `eth_tp_mdix_ctrl`: same as above.
- `link_mode_masks_nwords` (`__s8`): on `GLINKSETTINGS` the
  first call returns this negative (or zero) telling userspace
  how many `__u32` words follow in each of the three link-mode
  bitmaps. Userspace must then re-call with a buffer of the
  right size. The trailing `link_mode_masks[]` array holds:
  `map_supported[]`, `map_advertising[]`, `map_lp_advertising[]`,
  each `link_mode_masks_nwords` words long.
- `transceiver`, `master_slave_cfg`, `master_slave_state`,
  `rate_matching`: newer fields.
- `reserved[7]`.

#### struct ethtool_drvinfo  (ETHTOOL_GDRVINFO)
- `cmd`, `driver[32]`, `version[32]`, `fw_version[32]`,
  `bus_info[32]`, `erom_version[32]`, `reserved2[12]`,
  `n_priv_flags`, `n_stats`, `testinfo_len`, `eedump_len`,
  `regdump_len` (all `__u32`). The latter four are needed to
  size subsequent `GSTRINGS`, `GSTATS`, `TEST`, `GEEPROM`,
  `GREGS` calls.

#### struct ethtool_wolinfo  (ETHTOOL_GWOL / SWOL)
- `cmd`, `supported` (`__u32` bitmap of supported `WAKE_*`
  modes), `wolopts` (`__u32` currently enabled modes),
  `sopass[6]` (SecureOn password, only meaningful if
  `WAKE_MAGICSECURE` is set). `WAKE_PHY` = 1, `WAKE_UCAST` = 2,
  `WAKE_MCAST` = 4, `WAKE_BCAST` = 8, `WAKE_ARP` = 16,
  `WAKE_MAGIC` = 32, `WAKE_MAGICSECURE` = 64,
  `WAKE_FILTER` = 128.

#### struct ethtool_coalesce  (ETHTOOL_GCOALESCE / SCOALESCE)
24 `__u32` fields after `cmd`:

```
rx_coalesce_usecs          rx_max_coalesced_frames
rx_coalesce_usecs_irq      rx_max_coalesced_frames_irq
tx_coalesce_usecs          tx_max_coalesced_frames
tx_coalesce_usecs_irq      tx_max_coalesced_frames_irq
stats_block_coalesce_usecs use_adaptive_rx_coalesce
use_adaptive_tx_coalesce   pkt_rate_low
rx_coalesce_usecs_low      rx_max_coalesced_frames_low
tx_coalesce_usecs_low      tx_max_coalesced_frames_low
pkt_rate_high              rx_coalesce_usecs_high
rx_max_coalesced_frames_high tx_coalesce_usecs_high
tx_max_coalesced_frames_high rate_sample_interval
```

- IRQ coalescing rule: an interrupt fires when
  `usecs > 0 && elapsed >= usecs` **or**
  `max_frames > 0 && completions >= max_frames`.
- Both `usecs` and `max_frames` may not be zero simultaneously
  (the kernel rejects this; it would never fire an IRQ).
- To disable coalescing, set `usecs = 0` and `max_frames = 1`.
- `pkt_rate_low` / `pkt_rate_high` are thresholds in
  packets/second. `rate_sample_interval` is the adaptive
  sample period in seconds (must be non-zero when adaptive
  mode is on).
- Units: usecs for `*_usecs*`, packets for `*_frames*`,
  packets/second for `pkt_rate_*`, seconds for
  `rate_sample_interval`.

#### struct ethtool_ringparam  (ETHTOOL_GRINGPARAM / SRINGPARAM)
- `cmd`, `rx_max_pending` (RO), `rx_mini_max_pending` (RO),
  `rx_jumbo_max_pending` (RO), `tx_max_pending` (RO),
  `rx_pending`, `rx_mini_pending`, `rx_jumbo_pending`,
  `tx_pending` (all RW).
- Units: descriptors. Drivers may impose minimums not visible
  here; out-of-range writes return `-EINVAL`.

#### struct ethtool_pauseparam  (ETHTOOL_GPAUSEPARAM / SPAUSEPARAM)
- `cmd`, `autoneg` (`__u32`): 1 = autoneg pause, 0 = forced.
- `rx_pause` (`__u32`): 1 = receive pause frames honoured.
- `tx_pause` (`__u32`): 1 = transmit pause frames sent.

#### struct ethtool_channels  (ETHTOOL_GCHANNELS / SCHANNELS)
- `cmd`, `max_rx`, `max_tx`, `max_other`, `max_combined`
  (RO), `rx_count`, `tx_count`, `other_count`,
  `combined_count` (RW). A combined channel serves both RX
  and TX. The driver refuses combinations that exceed the
  maximums or its internal constraints (e.g. RX + combined
  may not exceed `max_rx + max_combined`).

#### struct ethtool_gfeatures / ethtool_sfeatures
`ETHTOOL_GFEATURES` returns the current state of every feature
bit in `features[]`, each element being a `struct
ethtool_get_features_block { __u32 available; __u32 requested;
__u32 active; }`. `ETHTOOL_SFEATURES` takes
`ethtool_set_features_block { __u32 changeable; __u32 requested;
}` — only bits set in `changeable` are written; the kernel
applies the requested mask and reports the result in a
follow-up `GFEATURES` call.

Feature bits are defined in `include/linux/netdev_features.h`
(e.g. `NETIF_F_RXCSUM`, `NETIF_F_HW_CSUM`, `NETIF_F_TSO`,
`NETIF_F_GRO`, `NETIF_F_GSO`, `NETIF_F_SG`). The legacy
on/off ioctls (`GRXCSUM`/`SRXCSUM`, `GTXCSUM`/`STXCSUM`,
`GSG`/`SSG`, `GTSO`/`STSO`, `GGRO`/`SGRO`, `GGSO`/`SGSO`,
`GFLAGS`/`SFLAGS`) are thin wrappers around the same feature
bits and exist for backwards compatibility. Modern tooling
should use `GFEATURES` / `SFEATURES`.

#### Statistic string / value retrieval
1. Call `ETHTOOL_GDRVINFO` to get `n_stats` (the number of
   statistics), `n_priv_flags`, `testinfo_len`, `eedump_len`,
   `regdump_len`.
2. Call `ETHTOOL_GSTRINGS` with `string_set = ETH_SS_STATS`
   and `len = n_stats * ETH_GSTRING_LEN` to get the names
   (each `ETH_GSTRING_LEN = 32` bytes, NUL-padded).
3. Call `ETHTOOL_GSTATS` with `n_stats` to get the matching
   `__u64[]` values.

`ETH_SS_STATS_STD`, `ETH_SS_STATS_ETH_PHY`, `ETH_SS_STATS_ETH_MAC`,
`ETH_SS_STATS_ETH_CTRL`, `ETH_SS_STATS_RMON` provide
standardised IEEE / RMON statistic names exposed via
`ethtool -S --groups`.

---

## PCIe / IOMMU

### PCI device sysfs layout

Each PCI device appears at `/sys/bus/pci/devices/<addr>/` as a
symlink to `/sys/devices/pci.../<addr>/`. The full attribute
list is created by `drivers/pci/pci-sysfs.c`. The most relevant
files for tuning:

#### /sys/bus/pci/devices/<addr>/vendor, device, subsystem_vendor, subsystem_device, revision, class
- Read: the corresponding config-space field as `0x%04x\n`
  (or `0x%02x\n` for revision, `0x%06x\n` for class). Built by
  the `pci_config_attr()` macro.
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/irq
- Read: the LSI/INTx IRQ number if no MSI is in use, or the
  first MSI vector if MSI (not MSI-X) is enabled. `0` if the
  device cannot generate INTx.
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/enable
- Read: decimal count of how many times the device has been
  enabled (`pci_enable_device` calls minus disables).
- Write: `1` to increment, `0` to decrement. Even at 0 some
  initialisation may not be reversed.
- Permissions: `0644`.

#### /sys/bus/pci/devices/<addr>/remove
- Write: `1` to hot-remove the device and its children. The
  device disappears from sysfs.
- Permissions: `0200`.

#### /sys/bus/pci/devices/<addr>/rescan
- Write: `1` to rescan this device's parent bus.
- Permissions: `0200`.

#### /sys/bus/pci/devices/<addr>/reset
- Write: `1` to perform a function-level reset (FLR or
  equivalent according to the reset-method list).
- Permissions: `0200`.
- Quirks: present only when at least one reset method is
  available. Read returns `-EINVAL`.

#### /sys/bus/pci/devices/<addr>/reset_method
- Read: newline-separated list of enabled reset methods in
  priority order, e.g. `bus\nflr\n`.
- Write: space-separated list to set the order, or `default`
  to restore, or empty string to disable.
- Permissions: `0644`.

#### /sys/bus/pci/devices/<addr>/resource
- Read: one line per BAR, showing start, end, flags in hex.
  RO.
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/resource0..resourceN, resourceN_wc
- Binary mmapable files mapping the corresponding BAR.
  `resourceN_wc` is the write-combining variant for
  prefetchable BARs. RW for I/O-port BARs, RO otherwise
  (writing requires `CAP_SYS_RAWIO`).
- Permissions: `0400` / `0600` depending on the BAR type.

#### /sys/bus/pci/devices/<addr>/rom
- Binary file containing the device expansion ROM. Disabled
  by default; write `1` to enable, `0` to disable. The device
  must be `enable`d before a rom read returns data.
- Permissions: `0600`.

#### /sys/bus/pci/devices/<addr>/config
- Binary file containing the device's config space. Size is
  256 bytes for legacy PCI devices, 4096 bytes for PCIe
  devices with extended config space (`pdev->cfg_size >
  PCI_CFG_SPACE_SIZE`). Read/write require `CAP_SYS_ADMIN` on
  open.
- Permissions: `0644`.
- Quirks: writes are subject to capability white-listing —
  certain config-space registers (e.g. BARs, command register
  MSI-X table) are write-protected to prevent the kernel's
  view from diverging from userspace's. Reads of
  write-protected areas return the kernel-visible value.

#### /sys/bus/pci/devices/<addr>/current_link_speed
- Read: the negotiated PCIe link speed as a string. Returned
  by `pci_speed_string()` from `pcie_link_speed[]`. Values
  seen in practice: `2.5 GT/s`, `5.0 GT/s`, `8.0 GT/s`,
  `16.0 GT/s`, `32.0 GT/s`, `64.0 GT/s`. PCIe 7.0 adds
  `128.0 GT/s`.
- Permissions: `0444`.
- Quirks: only present for PCIe devices (the `pcie_dev_attrs`
  attribute group). Read from `PCI_EXP_LNKSTA` CLS field.
  Returns `-EINVAL` if the read fails (e.g. device powered
  down).

#### /sys/bus/pci/devices/<addr>/current_link_width
- Read: negotiated link width as a decimal integer (1, 2, 4,
  8, 16, 32). Read from `PCI_EXP_LNKSTA` NLW field.
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/max_link_speed
- Read: the maximum link speed the device is capable of, as
  a string. Read from `PCI_EXP_LNKCAP` SLS field.
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/max_link_width
- Read: the maximum link width the device is capable of, as
  a decimal integer. Read from `PCI_EXP_LNKCAP` M LW field.
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/power_state
- Read: current PCI power state: `unknown`, `error`, `D0`,
  `D1`, `D2`, `D3hot`, `D3cold`.
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/numa_node
- Read: NUMA node the device is attached to, or `-1` if
  unknown. Initial value comes from ACPI `_PXM` or similar
  firmware source.
- Write: positive decimal integer. Sets `dev->numa_node`,
  taints the kernel with `TAINT_FIRMWARE_WORKAROUND`
  (intended as a firmware-bug workaround).
- Permissions: `0644` (only visible with `CONFIG_NUMA`).
- Quirks: writing does not reallocate already-allocated
  memory; for it to take effect the device's driver must be
  reloaded or the device re-bound.

#### /sys/bus/pci/devices/<addr>/d3cold_allowed
- Read: `1` if D3cold is permitted for this device, `0`
  otherwise. Defaults to `1` for endpoints, `0` for bridges
  that don't support D3cold.
- Write: `0` or `1`. Calls `pci_bridge_d3_update()` and
  `pm_runtime_resume()` after the change.
- Permissions: `0644` (visible only with `CONFIG_PM` and
  `CONFIG_ACPI`).
- Quirks: clearing this is the main userspace lever to keep
  a problematic device out of D3cold (e.g. a NVMe SSD that
  fails to resume). The setting propagates up the bridge
  hierarchy — D3cold for an endpoint requires its upstream
  bridge to also allow D3cold.

#### /sys/bus/pci/devices/<addr>/power/control
- Read: `on` or `auto`.
- Write: `on` (disable runtime PM) or `auto` (allow runtime
  PM). Same semantics as for any `struct device`.
- Permissions: `0644`.

#### /sys/bus/pci/devices/<addr>/power/wakeup
- Read: `enabled` or `disabled`.
- Write: `enabled` or `disabled`. Controls whether the
  device's PME is wired into the platform wake source.
- Permissions: `0644`.
- Quirks: only meaningful if the device can generate PME
  (most PCIe endpoints can).

#### /sys/bus/pci/devices/<addr>/power/autosuspend_delay_ms
- Read: autosuspend delay in milliseconds, `-1` if disabled.
- Write: decimal integer in milliseconds. `0` allowed.
- Permissions: `0644`.
- Quirks: only meaningful when `power/control = auto`.

#### /sys/bus/pci/devices/<addr>/msi_bus
- Read: `1` if MSI/MSI-X is permitted for this bus, `0`
  otherwise.
- Write: `0` or `1`. `0` disallows MSI/MSI-X for future
  drivers of this device and its children.
- Permissions: `0644`.

#### /sys/bus/pci/devices/<addr>/msi_irqs/
- Directory containing one file per MSI/MSI-X vector
  allocated to the device. Each file is named with the IRQ
  number and reads as `msi` or `msix`.

#### /sys/bus/pci/devices/<addr>/local_cpus, local_cpulist
- Read: the CPU mask (hex bitmask / cpulist) of CPUs close
  to this device (NUMA-local).
- Permissions: `0444`.

#### /sys/bus/pci/devices/<addr>/iommu_group
- Symlink to `/sys/kernel/iommu_groups/<N>/` for the IOMMU
  group this device belongs to. Created by
  `iommu_group_add_device()` in `drivers/iommu/iommu.c`.
  Devices that cannot do DMA are not in any group and the
  link is absent.
- Quirks: in passthrough mode (`iommu=pt`) the group still
  exists but contains an identity-mapped domain.

#### /sys/bus/pci/devices/<addr>/link/clkpm
#### /sys/bus/pci/devices/<addr>/link/l0s_aspm
#### /sys/bus/pci/devices/<addr>/link/l1_aspm
#### /sys/bus/pci/devices/<addr>/link/l1_1_aspm
#### /sys/bus/pci/devices/<addr>/link/l1_2_aspm
#### /sys/bus/pci/devices/<addr>/link/l1_1_pcipm
#### /sys/bus/pci/devices/<addr>/link/l1_2_pcipm
- Read: `1` if the corresponding ASPM substate is enabled
  on the link, `0` otherwise.
- Write: `y` / `1` / `on` to enable, `n` / `0` / `off` to
  disable. The store uses `kstrtobool()` so any of those
  spellings works.
- Permissions: `0644`.
- Quirks: present only when the endpoint supports ASPM on
  that link. Per-link overrides on top of the global
  `/sys/module/pcie_aspm/parameters/policy`.

#### /sys/bus/pci/devices/<addr>/sriov_totalvfs, sriov_numvfs
- `sriov_totalvfs` (RO): max VFs the PF can support.
- `sriov_numvfs` (RW): current VF count. Writing `0` disables
  all VFs; writing N enables N VFs. Must be `<= sriov_totalvfs`.

#### /sys/bus/pci/devices/<addr>/virtfnN, physfn, dep_link
- `virtfnN` is a symlink to the Nth virtual function (only on
  PFs with VFs enabled). `physfn` is a symlink back to the PF
  (only on VFs). `dep_link` links to a PF this device depends
  on (vendor-specific).

### ASPM global policy

#### /sys/module/pcie_aspm/parameters/policy
- Read: space-separated list of policy names with the active
  one bracketed:
  `[default] performance powersave powersupersave\n`.
- Write: one of `default`, `performance`, `powersave`,
  `powersupersave`. Matched with `sysfs_match_string()`
  (case-sensitive). Switching policy reconfigures every link
  via `pcie_config_aspm_link()`.
- Permissions: `0644`.
- Units: N/A (string).
- Quirks: this is a module param registered by
  `module_param_call(policy, ...)` in `drivers/pci/pcie/aspm.c`.
  The default policy is selected at compile time by
  `CONFIG_PCIEASPM_*`:
  - `CONFIG_PCIEASPM_DEFAULT`      → `default`
  - `CONFIG_PCIEASPM_PERFORMANCE`  → `performance`
  - `CONFIG_PCIEASPM_POWERSAVE`    → `powersave`
  - `CONFIG_PCIEASPM_POWER_SUPERSAVE` → `powersupersave`
  If ASPM is disabled at boot (`pcie_aspm=off`), the policy
  file exists but writes are ignored and the policy is fixed
  at `default`.

  Semantics:
  - `default`: trust BIOS / firmware config; do not change.
  - `performance`: disable all ASPM and Clock PM.
  - `powersave`: enable L0s and L1 wherever supported, even
    if the BIOS did not.
  - `powersupersave`: like `powersave` but also enable
    Clock PM and the deeper L1 substates (L1.1, L1.2).

#### pcie_aspm= kernel command line
- `pcie_aspm=off`: do not touch ASPM configuration at all,
  leave BIOS settings intact, disable the policy sysfs knob.
- `pcie_aspm=force`: enable ASPM even on devices that claim
  not to support it (use with caution; can break some
  hardware).

### PCI config space access

`/sys/bus/pci/devices/<addr>/config` is the canonical userspace
accessor for PCI config space. Size is 256 (legacy PCI) or 4096
(PCIe with extended config space) bytes. The file is mapped via
`pci_read_config` / `pci_write_config` in `pci-sysfs.c`.

Reads of any size up to the device's `cfg_size` are accepted.
Writes are restricted:

- The kernel rejects writes to registers it considers
  "owned" (command register, BARs, MSI capability, MSI-X
  table, etc.). The exact set depends on the kernel version
  and the `HAVE_PCI_SET_POWER_STATE` / `PCI_DEV_FLAGS_*`
  flags.
- A successful write returns the number of bytes written; a
  rejected write returns `-EPERM` or `-EINVAL`.

For sub-register access, `setpci` parses the address format
`<device>.<offset>[+<size>][:<mask>]` and reads-modifies-writes
through this file.

### IOMMU

#### /sys/kernel/iommu_groups/ structure
Each IOMMU group is a directory `/sys/kernel/iommu_groups/<N>/`
containing:

- `devices/` — subdirectory with one symlink per device in
  the group, named after the device's bus-specific path
  (e.g. `0000:00:02.0` for PCI).
- `name` — group name (if assigned by the IOMMU driver),
  readable as a string.
- `reserved_regions` — one line per reserved IOVA region:
  `start - end  type` where `type` is one of `direct`,
  `direct-relaxable`, `reserved`, `msi`.
- `type` — the type of the default domain attached to the
  group. Read returns one of `unknown`, `blocked`, `identity`,
  `unmanaged`, `DMA`, `DMA-FQ`. Write allows switching the
  default domain type at runtime (requires the group to have
  no driver bound; subject to `CONFIG_IOMMU_DEFAULT_PASSTHROUGH`
  and other restrictions).

#### /sys/bus/pci/devices/<addr>/iommu
- Symlink to the IOMMU device (e.g. `/sys/devices/virtual/iommu/intel-iommu`)
  managing this PCI device. Created by `iommu_device_link()`
  in `drivers/iommu/iommu-sysfs.c`.

#### /sys/class/iommu/<name>/devices/
- Subdirectory containing symlinks to every device managed by
  the named IOMMU (intel-iommu, amd-iommu, arm-smmu-v3, ...).
- The class is registered by `iommu-sysfs.c::iommu_dev_init()`.

#### IOMMU kernel parameters

Defined in `Documentation/admin-guide/kernel-parameters.txt`.

##### iommu=
- `off` — don't use any IOMMU.
- `force` — use the hardware IOMMU even when not needed.
- `noforce` (default) — don't force IOMMU usage when not
  needed.
- `pt` — passthrough mode (identity mapping). Equivalent to
  `iommu.passthrough=1`. Devices get 1:1 IOVA → physical
  mapping, bypassing dynamic translation; the IOMMU still
  isolates when explicit unmaps happen.
- `nopt` — translated mode (default for IOMMU-aware systems).
- `soft` — use software bounce buffering (SWIOTLB) instead of
  a hardware IOMMU. Default on Intel when IOMMU is not
  available.
- `merge`, `nomerge`, `biomerge` — scatter-gather merge
  options (mostly historical).
- `panic` / `nopanic` — what to do on IOMMU overflow.
- `usedac` — use DAC (64-bit) addressing on VIA bridges.
- AMD Gart-specific: `<size>`, `allowed`, `force`,
  `fullflush`, `nofullflush`, `memaper=<order>`, `noaperture`,
  `noagp`.

##### iommu.strict=
- `0` — lazy / deferred TLB invalidation. DMA unmaps defer
  the IOTLB flush to a timer-driven batch. Higher throughput,
  weaker isolation (an attacker who controls a malicious
  device may access unmapped pages for the brief window).
- `1` — strict mode. Every unmap immediately flushes the
  IOTLB. Lower throughput, stronger isolation. Default on
  most distros for security reasons.
- Effective on Intel VT-d, AMD-Vi, ARM SMMU, s390.
- Equivalent: `intel_iommu=strict`, `amd_iommu=fullflush`
  (both deprecated aliases for `iommu.strict=1`).

##### iommu.passthrough=
- `0` (default) — use translated (DMA) mode. Devices get a
  dynamically-allocated IOVA space; the IOMMU enforces
  isolation.
- `1` — passthrough mode. Devices get identity mapping; no
  DMA translation. Disables isolation. Useful for
  performance-obsessed setups with trusted hardware (single
  tenant, no untrusted PCIe hotplug).

##### iommu.dma_mode=
- Newer alternative spelling of the above: `lazy`, `strict`,
  `passthrough`, `translated`.

##### intel_iommu=
- `on` / `off` — enable / disable Intel VT-d.
- `igfx_off` — bypass the IOMMU for the integrated GPU (legacy
  workaround).
- `strict` — deprecated, = `iommu.strict=1`.
- `sp_off` — disable super-page (2 MiB / 1 GiB) support.
- `sm_on` / `sm_off` — enable / disable scalable mode
  (first-stage / nested translation).
- `tboot_noforce` — don't let tboot force the IOMMU on.

##### amd_iommu=
- `off` — don't initialise any AMD IOMMU.
- `fullflush` — deprecated, = `iommu.strict=1`.
- `force_isolation` — never lift isolation requirements even
  when the driver asks. Does not override `iommu=pt`.
- `force_enable` — force-enable on platforms known to be
  buggy.
- `pgtbl_v1` / `pgtbl_v2` — choose the page-table format.
- `irtcachedis` — disable interrupt-remapping-table caching.
- `nohugepages` — limit page sizes to 4 KiB.
- `v2_pgsizes_only` — limit v1 page-table page sizes to
  4 KiB / 2 MiB / 1 GiB.

##### amd_iommu_intr=
- `legacy` or `vapic` — interrupt-remapping mode.

##### intremap=
- `on` / `off` — enable / disable interrupt remapping.
- `nosid` — disable Source ID checking.
- `no_x2apic_optout` — ignore BIOS x2APIC opt-out.
- `nopost` — disable interrupt posting.
- `posted_msi` — deliver MSIs as posted interrupts.

#### /proc/sys/kernel/iommu_* (where present)
Some platforms expose extra IOMMU knobs under `/proc/sys/kernel/`
or `/sys/module/<driver>/parameters/`. The most portable ones
are the kernel-command-line parameters above.

### /sys/bus/pci/devices/<addr>/numa_node revisited

The same `numa_node` attribute is created by the PCI core for
every PCI device (visible with `CONFIG_NUMA`). The NVMe
controller duplicates this value at
`/sys/class/nvme/nvmeN/numa_node` for convenience. The
namespace multipath code adds `numa_nodes` (plural, with an
"s") at `/sys/block/<dev>/device/numa_nodes` — a *list* of
NUMA nodes across all paths in a multipath group, rather than
a single node.

---

## Quick reference: units and scales

| quantity                | unit         | where                                           |
|-------------------------|--------------|-------------------------------------------------|
| read_ahead_kb           | kB           | `queue/read_ahead_kb`                           |
| max_sectors_kb          | kB           | `queue/max_sectors_kb`, `max_hw_sectors_kb`     |
| logical/physical block  | bytes        | `queue/logical_block_size`, `physical_block_size`|
| discard_max_bytes       | bytes        | `queue/discard_max_bytes`, `discard_max_hw_bytes`|
| discard_granularity     | bytes        | `queue/discard_granularity`                     |
| io_min / io_opt         | bytes        | `queue/minimum_io_size`, `optimal_io_size`      |
| nr_requests             | requests     | `queue/nr_requests`                             |
| queue_depth (SCSI)      | commands     | `device/queue_depth`                            |
| io_timeout              | ms           | `queue/io_timeout`                              |
| wbt_lat_usec            | µs           | `queue/wbt_lat_usec`                            |
| autosuspend_delay_ms    | ms           | `device/power/autosuspend_delay_ms`             |
| nvme kato               | seconds      | `/sys/class/nvme/<c>/kato`                       |
| nvme ctrl_loss_tmo      | seconds      | `/sys/class/nvme/<c>/ctrl_loss_tmo`              |
| stat read_sectors ...   | 512-byte sectors | `/sys/block/<dev>/stat`                     |
| stat read_ticks ...     | ms           | `/sys/block/<dev>/stat`                         |
| NIC speed               | Mbit/s       | `class/net/<iface>/speed`                       |
| NIC mtu                 | bytes        | `class/net/<iface>/mtu`                         |
| NIC tx_queue_len        | packets      | `class/net/<iface>/tx_queue_len`                |
| rps_cpus / xps_cpus     | hex bitmask  | `queues/rx-N/rps_cpus`, `queues/tx-N/xps_cpus`  |
| rps_flow_cnt            | flows        | `queues/rx-N/rps_flow_cnt`                      |
| ethtool coalescing usecs| µs           | `ETHTOOL_GCOALESCE` / `SCOALESCE`               |
| ethtool pkt_rate_low    | packets/sec  | `ETHTOOL_GCOALESCE` / `SCOALESCE`               |
| ethtool rate_sample_interval | seconds | `ETHTOOL_GCOALESCE` / `SCOALESCE`               |
| ethtool ring pending    | descriptors  | `ETHTOOL_GRINGPARAM` / `SRINGPARAM`             |
| PCI link speed          | GT/s         | `current_link_speed`, `max_link_speed`          |
| PCI link width          | lanes (int)  | `current_link_width`, `max_link_width`          |
| PCI config space        | bytes (binary)| `config` (256 or 4096)                         |
| IOMMU group ID          | integer      | `/sys/kernel/iommu_groups/<N>/`                 |

## Quick reference: per-feature tuning knobs

| feature                  | where (sysfs)                                            |
|--------------------------|----------------------------------------------------------|
| block I/O scheduler      | `/sys/block/<dev>/queue/scheduler`                       |
| block tag depth          | `/sys/block/<dev>/queue/nr_requests`                     |
| block read-ahead         | `/sys/block/<dev>/queue/read_ahead_kb`                   |
| NVMe HW queue depth      | `/sys/module/nvme/parameters/io_queue_depth`             |
| SCSI queue depth         | `/sys/block/<dev>/device/queue_depth`                    |
| SCSI cache mode          | `/sys/block/<dev>/device/cache_type`                     |
| NVMe / virtio write cache| `/sys/block/<dev>/queue/write_cache`                     |
| discard support          | `queue/discard_max_bytes`, `queue/discard_granularity`   |
| WBT latency target       | `/sys/block/<dev>/queue/wbt_lat_usec`                    |
| NIC speed / duplex       | `/sys/class/net/<iface>/{speed,duplex}`                  |
| NIC MTU / queue len      | `/sys/class/net/<iface>/{mtu,tx_queue_len}`              |
| NIC offloads             | `ethtool` ioctl `GFEATURES` / `SFEATURES` (and legacy    |
|                          | `GTXCSUM`/`STXCSUM`, `GRXCSUM`/`SRXCSUM`, `GTSO`/`STSO`,  |
|                          | `GSG`/`SSG`, `GGRO`/`SGRO`)                               |
| NIC coalescing           | `ethtool` ioctl `GCOALESCE` / `SCOALESCE`                |
| NIC ring sizes           | `ethtool` ioctl `GRINGPARAM` / `SRINGPARAM`              |
| NIC channels             | `ethtool` ioctl `GCHANNELS` / `SCHANNELS`                |
| NIC pause frames         | `ethtool` ioctl `GPAUSEPARAM` / `SPAUSEPARAM`            |
| NIC wake-on-LAN          | `ethtool` ioctl `GWOL` / `SWOL`                          |
| NIC driver / fw info     | `ethtool` ioctl `GDRVINFO`                               |
| NIC stats                | `ethtool` ioctl `GSTATS` (+ `GSTRINGS`)                  |
| IRQ affinity             | `/proc/irq/<N>/{smp_affinity,smp_affinity_list}`         |
| RPS / XPS                | `/sys/class/net/<iface>/queues/{rx,tx}-<N>/{rps,xps}_cpus`|
| RFS flow count           | `/sys/class/net/<iface>/queues/rx-<N>/rps_flow_cnt`      |
|                          | + `/proc/sys/net/core/rps_sock_flow_entries`             |
| PCIe ASPM (global)       | `/sys/module/pcie_aspm/parameters/policy`                |
| PCIe ASPM (per-link)     | `/sys/bus/pci/devices/<addr>/link/{l0s,l1,...}_aspm`      |
| PCIe link speed / width  | `/sys/bus/pci/devices/<addr>/{current,max}_link_{speed,width}` |
| PCIe power state         | `/sys/bus/pci/devices/<addr>/power_state`                |
| PCIe D3cold              | `/sys/bus/pci/devices/<addr>/d3cold_allowed`             |
| PCIe runtime PM          | `/sys/bus/pci/devices/<addr>/power/control`              |
| PCIe wake                | `/sys/bus/pci/devices/<addr>/power/wakeup`               |
| PCIe autosuspend         | `/sys/bus/pci/devices/<addr>/power/autosuspend_delay_ms` |
| PCIe config space        | `/sys/bus/pci/devices/<addr>/config`                     |
| PCIe NUMA                | `/sys/bus/pci/devices/<addr>/numa_node`                  |
| IOMMU group              | `/sys/bus/pci/devices/<addr>/iommu_group` →              |
|                          | `/sys/kernel/iommu_groups/<N>/`                          |
| IOMMU domain type        | `/sys/kernel/iommu_groups/<N>/type`                      |
| IOMMU passthrough / strict | kernel cmdline `iommu=pt`, `iommu.strict=1`            |
| Intel IOMMU              | `intel_iommu={on,off,strict,sm_on,...}`                  |
| AMD IOMMU                | `amd_iommu={off,fullflush,force_isolation,...}`          |
