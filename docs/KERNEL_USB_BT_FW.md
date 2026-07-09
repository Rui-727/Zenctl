# Linux Kernel Interfaces for USB, Bluetooth, Wireless, and Firmware/BIOS

Reference for the Zenctl domains 9 (USB / Bluetooth / Wireless) and 10
(Firmware / BIOS). Every interface below is mapped to the exact kernel
source path, the exact sysfs / procfs / ioctl / netlink path, the read
format, the write format, the on-disk permissions, the units, and any
quirks.

All paths are kernel-version-stable ABI as of Linux 6.6 LTS unless
otherwise noted. Where a path is marked *deprecated* or *removed* the
replacement is given. The kernel source snapshot used as the
authoritative reference is at commit
`0e35b9b Merge tag 'mm-hotfixes-stable-2026-07-06-17-49' of
git://git.kernel.org/pub/scm/linux/kernel/git/akpm/mm` (torvalds/linux
master, July 2026).

---

## 1. USB Domain

### 1.1 USB device sysfs tree

Every USB device (host, hub, function, interface) lives under:

```
/sys/bus/usb/devices/<id>/
```

`<id>` is one of:

| Form | Meaning |
|---|---|
| `usbN` | Root hub on bus N (a host controller). |
| `N-P` | Device on bus N, port P (or `N-P.Q.R` for hubs of hubs). |
| `N-P:Config.Interface` | USB interface (e.g. `1-2:1.0`). |

Each USB device directory exposes the standard attributes documented in
`Documentation/ABI/testing/sysfs-bus-usb` and `Documentation/ABI/stable/sysfs-bus-usb`.
The full set relevant to Zenctl:

| Path | R/W | Format | Units / Notes |
|---|---|---|---|
| `idVendor` | RO | hex string (4 chars) | USB vendor ID, e.g. `046d`. |
| `idProduct` | RO | hex string (4 chars) | USB product ID, e.g. `c52b`. |
| `bcdDevice` | RO | hex string | Device release number (BCD). |
| `bDeviceClass` | RO | hex string | Device class code. |
| `bDeviceSubClass` | RO | hex string | Device subclass code. |
| `bDeviceProtocol` | RO | hex string | Device protocol code. |
| `bConfigurationValue` | RW | decimal | Active config value. Write `0` or `-1` to unconfigure, other values to switch configurations. Spec-violating devices with a configuration value of `0` make `0` install that configuration instead of unconfiguring; `-1` always unconfigures. |
| `bNumConfigurations` | RO | decimal | Number of supported configurations. |
| `bNumInterfaces` | RO | decimal | Number of interfaces in active configuration. |
| `bMaxPacketSize0` | RO | decimal | EP0 max packet size (bytes). |
| `bMaxPower` | RO | decimal | Max power draw of the active configuration, **mA** (note: spec value is 2 mA units pre-USB 3.0; the kernel reports the decoded mA). |
| `bmAttributes` | RO | hex string | Configuration attributes bitmap. |
| `manufacturer` | RO | string | iManufacturer string descriptor (empty if device has none). |
| `product` | RO | string | iProduct string descriptor. |
| `serial` | RO | string | iSerialNumber string descriptor. |
| `version` | RO | string | `bcdUSB`, e.g. `2.00`, `3.00`, `3.10`. |
| `speed` | RO | decimal Mbps | One of `1.5`, `12`, `480`, `5000`, `10000`, `20000`. `0` if unknown. |
| `rx_lanes` / `tx_lanes` | RO | decimal | Lane count (USB 3.2 dual-lane, SSIC). Pre-USB 3.2 devices report `1`. |
| `busnum` | RO | decimal | USB bus number. |
| `devnum` | RO | decimal | Device address on the bus. |
| `devpath` | RO | string | Port path, e.g. `2.1.3`. |
| `maxchild` | RO | decimal | Number of downstream ports (hubs only; `0` for functions). |
| `urbnum` | RO | decimal | URB count submitted for the device lifetime. |
| `ltm_capable` | RO | `yes` / `no` | Latency Tolerance Messaging support (USB 3.0). |
| `configuration` | RO | string | Active configuration's iConfiguration string. |
| `descriptors` | RO | binary | Cached device + all config descriptors (bus-endian). `wTotalLength` in config descriptors is untrustworthy; walk by `bLength`. |
| `bos_descriptors` | RO | binary | Cached BOS descriptor (USB 2.0.1+). Not requested if `bcdUSB < 0x0201`. |
| `avoid_reset_quirk` | RW | `0` / `1` | When `1`, kernel will not use reset for error recovery on this device (used by `usb_modeswitch` to handle devices that morph on reset). |
| `supports_autosuspend` | RO | `0` / `1` | `1` if the bound interface driver sets `supports_autosuspend` (or no driver is bound). |
| `removable` | RO | `unknown` / `fixed` / `removable` | ACPI `_PLD` hint if available. |
| `rx_lanes`, `tx_lanes` | RO | decimal | See above. |

Interface directories (`N-P:C.I/`) additionally expose:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `bInterfaceClass` | RO | hex string | Interface class. |
| `bInterfaceSubClass` | RO | hex string | Interface subclass. |
| `bInterfaceProtocol` | RO | hex string | Interface protocol. |
| `bInterfaceNumber` | RO | hex string | Interface number. |
| `bAlternateSetting` | RO | decimal | Active alt setting. |
| `bNumEndpoints` | RO | decimal | Endpoint count. |
| `authorized` | RW | `0` / `1` | Per-interface authorize. See section 1.5. |
| `driver` -> symlink | RO | | Bound driver module. |
| `ep_<N>/` | RO | dir | Per-endpoint dir with `bEndpointAddress`, `bInterval`, `bLength`, `bmAttributes`, `direction` (`in`/`out`/`both`), `interval` (decoded µs or ms), `type` (`Control`/`Isoc`/`Bulk`/`Interrupt`), `wMaxPacketSize`. |

### 1.2 USB device power management (runtime PM)

Kernel source: `drivers/usb/core/driver.c`, `drivers/base/power/sysfs.c`,
`Documentation/driver-api/usb/power-management.rst`.

The runtime PM attributes live in `power/` under every USB device
directory. These are defined by the PM core in
`drivers/base/power/sysfs.c` (the generic set) plus USB-specific
extensions registered by `drivers/usb/core/sysfs.c`:

| Path | R/W | Format | Units | Notes |
|---|---|---|---|---|
| `power/control` | RW | `on` / `auto` | — | `on` blocks runtime autosuspend (and resumes the device if suspended). `auto` lets the kernel autosuspend. Default is `on` for non-hub devices; hubs default to `auto`. |
| `power/autosuspend_delay_ms` | RW | signed int | ms | Idle time before autosuspend. `0` = suspend as soon as idle. Negative = never suspend. Default `2000` (2 s). Only effective when the driver calls `pm_runtime_use_autosuspend()` — USB core does this for all USB devices. |
| `power/runtime_status` | RO | `active` / `suspended` / `unsupported` / `suspended` | — | Current runtime PM state. |
| `power/runtime_active_time` | RO | decimal | ms | Time the device has been active since boot (monotonic, u64). |
| `power/runtime_suspended_time` | RO | decimal | ms | Time the device has been suspended since boot. |
| `power/runtime_usage` | RO | decimal | — | PM usage counter (active reference count). |
| `power/runtime_active_kids` | RO | decimal | — | Active child count. |
| `power/runtime_enabled` | RO | `enabled` / `disabled` / `forbidden` | — | Whether runtime PM is enabled. |
| `power/active_duration` | RO | decimal | ms | USB-specific. Total active time since the device was connected (not since boot). Wraps at u64 max. |
| `power/connected_duration` | RO | decimal | ms | USB-specific. Total connected time. `100 * active_duration / connected_duration` gives the active-time percentage. |
| `power/wakeup` | RW | `enabled` / `disabled` / (empty) | — | If empty, device has no remote-wakeup capability. Change only takes effect on the next suspend. |
| `power/wakeup_count` | RO | decimal | — | Wakeup events signalled. |
| `power/wakeup_active_count` | RO | decimal | — | Wakeup events currently being processed. |
| `power/wakeup_abort_count` | RO | decimal | — | Wakeup events aborted. |
| `power/wakeup_expire_count` | RO | decimal | — | Wakeup events whose timer expired. |
| `power/wakeup_active` | RO | `0` / `1` | — | Wakeup in progress. |
| `power/wakeup_total_time_ms` | RO | decimal | ms | Total time spent processing wakeups. |
| `power/wakeup_max_time_ms` | RO | decimal | ms | Longest single wakeup. |
| `power/wakeup_last_time_ms` | RO | decimal | ms | Timestamp of last wakeup. |
| `power/wakeup_prevent_sleep_time_ms` | RO | decimal | ms | Time wakeups prevented system sleep. |
| `power/async` | RW | `enabled` / `disabled` | — | Allow async suspend/resume. |
| `power/pm_qos_resume_latency_us` | RW | signed int | µs | PM QoS resume latency constraint. |
| `power/pm_qos_latency_tolerance_us` | RW | signed int | µs | PM QoS latency tolerance. `0` = no constraint. |
| `power/pm_qos_no_power_off` | RW | `0` / `1` | — | `1` (default) forbids the device from powering off. `0` allows power-off. Only relevant for ports and other devices that can be physically powered down. |
| `power/persist` | RW | `0` / `1` | — | USB-Persist. See 1.6. |

Deprecated (do not use, removed-soon):

| Path | Status |
|---|---|
| `power/level` | `on` / `auto` / `suspend`. Replaced by `power/control` in 2.6.35. Removed from stable ABI; documented in `Documentation/ABI/obsolete/sysfs-bus-usb`. |
| `power/autosuspend` | Same as `autosuspend_delay_ms` but in seconds. Replaced in 2.6.37. |

#### 1.2.1 USB hardware link power management (LPM)

xHCI controllers negotiate L1 (USB 2.0) or U1/U2 (USB 3.0) hardware
link states with the device. The relevant sysfs knobs:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `power/usb2_hardware_lpm` | RW | `enabled` / `disabled` (write `y`/`Y`/`1`/`n`/`N`/`0`) | Present only when an LPM-capable USB 2.0 device is on an xHCI 1.0 host and the LPM test passed. |
| `power/usb3_hardware_lpm_u1` | RW | `enabled` / `disabled` | USB 3.0 U1 link state. |
| `power/usb3_hardware_lpm_u2` | RW | `enabled` / `disabled` | USB 3.0 U2 link state. |
| `power/usb2_lpm_l1_timeout` | RW | `0..65535` | µs. L1 inactivity timer (LPM timer). |
| `power/usb2_lpm_besl` | RW | `0..15` | Best Effort Service Latency value used by the host when the device has no preferred BESL. See USB 2.0 LPM ECN section 4.10. |

#### 1.2.2 USB autosuspend defaults

The default `autosuspend_delay_ms` for new devices is set by the
`usbcore.autosuspend` module parameter:

```
/sys/module/usbcore/parameters/autosuspend       # signed int, seconds. Default 2.
```

Note that this is **seconds** (it's the old module parameter); the
per-device `autosuspend_delay_ms` sysfs file is **milliseconds**.
Writing `-1` here disables autosuspend for all newly registered
devices. Existing devices keep their old value.

Per-driver autosuspend tuning: many USB drivers export their own
`no_autosuspend` / `enable_autosuspend` module params, e.g.:

```
/sys/module/btusb/parameters/enable_autosuspend      # bool
/sys/module/usbhid/parameters/ignoreled              # bool, not autosuspend
```

Search `/sys/module/<drvname>/parameters/` for each driver.

### 1.3 USB device authorization (lockdown)

Kernel source: `Documentation/usb/authorization.rst`,
`drivers/usb/core/sysfs.c`. Allows root to gate which USB devices may
be enumerated.

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/bus/usb/devices/<dev>/authorized` | RW | `0` / `1` | `0` deauthorizes: the device's interfaces are unbound and unavailable to drivers. `1` authorizes. Default `1` for wired USB. |
| `/sys/bus/usb/devices/usbN/authorized_default` | RW | `0` / `1` / `2` | Per-host-controller default. `0` = lockdown (new devices deauthorized), `1` = authorize new devices (default), `2` = authorize only devices on internal ports (ACPI `_PLD` says `hotplug == false`). |
| `/sys/bus/usb/devices/<iface>/authorized` | RW | `0` / `1` | Per-interface authorize. |
| `/sys/bus/usb/devices/usbN/interface_authorized_default` | RW | `0` / `1` | Per-host default for new interfaces. Default `1`. |

To reprobe a deauthorized interface, write the interface name to
`/sys/bus/usb/drivers_probe`.

### 1.4 USB device removal and reset

#### 1.4.1 Logical remove

```
echo 1 > /sys/bus/usb/devices/<dev>/remove
```

Tells the kernel to forget the device. The port stays powered (the
device will re-enumerate if it's still physically connected). Requires
root.

#### 1.4.2 USBDEVFS_RESET ioctl

For a stronger reset that re-initialises the device without
unplugging it, use the `USBDEVFS_RESET` ioctl on the
`/dev/bus/usb/<bus>/<dev>` character device:

```c
#include <linux/usbdevice_fs.h>
int fd = open("/dev/bus/usb/003/004", O_WRONLY);
int rc = ioctl(fd, USBDEVFS_RESET, 0);
```

`USBDEVFS_RESET` is `_IO('U', 20)` (defined in
`include/uapi/linux/usbdevice_fs.h`). It performs a USB port reset on
the device. The device keeps its address and configuration, but all
endpoint state is cleared. Drivers' `pre_reset` and `post_reset`
methods are invoked.

Use case: a device has wedged but is still on the bus. Less invasive
than unbind + remove + replug.

Related ioctls on the same fd:

| ioctl | Magic | Number | Effect |
|---|---|---|---|
| `USBDEVFS_RESET` | `'U'` | 20 | Reset the device (port reset). |
| `USBDEVFS_GET_CAPABILITIES` | `'U'` | 26 | Read supported cap bits (`USBDEVFS_CAP_*`). |
| `USBDEVFS_DROP_PRIVILEGES` | `'U'` | 30 | Drop the fd's privileges to a subset (capability mask). |
| `USBDEVFS_FORBID_SUSPEND` | `'U'` | 33 | Forbid runtime suspend while the fd is open. |
| `USBDEVFS_ALLOW_SUSPEND` | `'U'` | 34 | Re-allow runtime suspend. |
| `USBDEVFS_WAIT_FOR_RESUME` | `'U'` | 35 | Block until the device is resumed. |
| `USBDEVFS_SUBMITURB` / `USBDEVFS_REAPURB` / `USBDEVFS_REAPURBNDELAY` | `'U'` | 10 / 12 / 13 | Submit / reap URBs. |
| `USBDEVFS_CONTROL` | `'U'` | 0 | Synchronous control transfer (`struct usbdevfs_ctrltransfer`). |
| `USBDEVFS_BULK` | `'U'` | 2 | Synchronous bulk transfer. |
| `USBDEVFS_CLAIMINTERFACE` / `USBDEVFS_RELEASEINTERFACE` | `'U'` | 15 / 16 | Claim / release an interface. |
| `USBDEVFS_SETCONFIGURATION` | `'U'` | 5 | Set the active configuration. |
| `USBDEVFS_SETINTERFACE` | `'U'` | 4 | Set alt setting. |
| `USBDEVFS_CLEAR_HALT` | `'U'` | 21 | Clear a stall on an endpoint. |
| `USBDEVFS_DISCONNECT_CLAIM` | `'U'` | 27 | Atomically disconnect existing driver and claim. |
| `USBDEVFS_CONNINFO_EX(len)` | `'U'` | 32 | Extended connection info (bus/dev/speed/port chain). |
| `USBDEVFS_GET_SPEED` | `'U'` | 31 | Get device speed (returns `USB_SPEED_*`). |

The full ioctl list is in `include/uapi/linux/usbdevice_fs.h`.

Permissions: `/dev/bus/usb/<bus>/<dev>` is created by `devtmpfs` /
`udev` with mode `0664`, group `root` (or a vendor-specific group such
as `plugdev`). The fd requires `O_RDWR` for any transfer ioctl.

#### 1.4.3 Set/Clear port feature (port reset from userspace)

Not directly exposed via sysfs. Use `USBDEVFS_RESET` (above) or
`uhubctl` (which uses libusb to send a `SetPortFeature(PORT_RESET)`
control request to the hub).

### 1.5 USB port power control

For hubs that advertise per-port power switching (`wHubCharacteristics`
logical power switching mode != ganged), the kernel exposes the port
device separately:

```
/sys/bus/usb/devices/<hub>/<hub_iface>/<port>/
```

e.g. `/sys/bus/usb/devices/usb3/3-1/3-1:1.0/3-1-port1/`.

Port device attributes (from `drivers/usb/core/port.c`):

| Path | R/W | Format | Notes |
|---|---|---|---|
| `power/pm_qos_no_power_off` | RW | `0` / `1` | `1` (default) keeps port powered. `0` allows PM runtime to power off the port (drops VBUS on hubs that support per-port switching). |
| `power/runtime_status` | RO | `active` / `suspended` | Port power state. |
| `connect_type` | RO | `hotplug` / `hardwired` / `not used` / `unknown` | ACPI `_PLD` hint. `unknown` if no ACPI info. |
| `location` | RO | hex int | ACPI physical location code. |
| `state` | RO | `not-attached` / `attached` / `powered` / `reconnecting` / `unauthenticated` / `default` / `addressed` / `configured` / `suspended` | Current port state. `poll()`-able. |
| `quirks` | RW | bitmask | Bit 0: use the old enumeration scheme (one reset). Bit 1: reduce `TRSTRCY` from 50 ms to 10 ms. |
| `over_current_count` | RO | decimal | Over-current events on this port. `poll()`-able; udev events emit `OVER_CURRENT_PORT` / `OVER_CURRENT_COUNT`. |
| `disable` | RW | `0` / `1` | Disable the port (drop VBUS if hub supports power switching). |
| `early_stop` | RW | `0` / `1` | Limit enumeration retries to 2 and stop attempting on failure. |
| `usb3_lpm_permit` | RW | `0` / `u1` / `u2` / `u1_u2` | Permit U1, U2, both, or neither on a USB 3.0 port. USB 3.x ports only. |
| `peer` -> | symlink | | For combo hi-speed + superspeed ports, points to the other half of the same physical connector. |

Pre-requisites for poweroff (per
`Documentation/driver-api/usb/power-management.rst`):

```
echo 0 > <port>/power/pm_qos_no_power_off
echo 0 > <port>/peer/power/pm_qos_no_power_off        # if peer exists
echo auto > <port>/power/control                       # default
echo auto > <child>/power/control
echo 1 > <child>/power/persist                         # default
```

Wakeup-capable child devices block poweroff; unbind the child's
interface drivers to clear the wakeup capability.

### 1.6 USB Persist

Kernel source: `Documentation/driver-api/usb/persist.rst`,
`drivers/usb/core/sysfs.c`, Kconfig `CONFIG_USB_PERSIST`.

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/bus/usb/devices/<dev>/power/persist` | RW | `0` / `1` | When `1` (default), the kernel preserves the device's state and config across a power-session loss (host controller reset, hibernation, port power cycle). The driver's `reset_resume` callback runs. Hubs always have persist on; no `persist` file is created for hubs. |

The problem it solves: USB mass-storage devices lose their mounted
filesystem when the host controller is reset during suspend. With
`persist=1`, the kernel reinitialises the device after resume and
keeps the mounted filesystem intact.

### 1.7 USB host controller sysfs

The root hub at `/sys/bus/usb/devices/usbN/` is a hub device, so all
the device attributes above apply. Host controller-specific stuff is
exposed at the PCI device level (`/sys/bus/pci/devices/<addr>/`) —
ASPM, ACS, etc., covered in the PCIe doc.

`/sys/bus/usb/devices/usbN/authorized_default` and
`interface_authorized_default` (see 1.3) are the host-controller-level
authorization defaults.

### 1.8 USB driver sysfs

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/bus/usb/drivers/<drv>/new_id` | RW | `idVendor idProduct [bInterfaceClass [RefIdVendor RefIdProduct]]` | Dynamically add a device ID to a driver's match table. Reading lists the dynamic IDs. |
| `/sys/bus/usb/drivers/<drv>/remove_id` | W | `idVendor idProduct` | Remove a dynamic ID. |
| `/sys/bus/usb/drivers/<drv>/bind` | W | device ID string | Bind driver to device. |
| `/sys/bus/usb/drivers/<drv>/unbind` | W | device ID string | Unbind driver from device. |
| `/sys/bus/usb/drivers_probe` | W | interface ID string | Trigger driver probe for an interface (used after re-authorizing). |

### 1.9 USB core module parameters

```
/sys/module/usbcore/parameters/autosuspend           # int seconds. Default 2.
/sys/module/usbcore/parameters/old_scheme_first      # bool. Use old enumeration scheme first.
/sys/module/usbcore/parameters/usbfs_memory_mb       # int MB. Default 16 (per-uid USBFS buffer quota; 0 = no limit).
/sys/module/usbcore/parameters/initial_descriptor_timeout__ms   # unsigned. Default 5000.
/sys/module/usbcore/parameters/autosuspend_delay_ms  # not present; use per-device file.
```

### 1.10 USBFS (usbdevfs)

Mount point: usually not mounted anymore; the character-device interface at `/dev/bus/usb/<bus>/<dev>` is preferred. The old `/proc/bus/usb/` mount is gated by `CONFIG_USB_DEVICEFS` (deprecated).

Permissions on `/dev/bus/usb/<bus>/<dev>`: `0664` root:root by
default; `udev` rules may relax the group (e.g. to `plugdev` or
`usbusers`).

### 1.11 udev integration

`udevadm monitor --kernel --udev` shows USB events. Useful udev
properties for power rules:

- `ACTION=add|remove|change`
- `SUBSYSTEM=usb`
- `KERNEL=<id>` (e.g. `1-2:1.0`)
- `ATTR{idVendor}`, `ATTR{idProduct}`, `ATTR{serial}`
- `ENV{ID_USB_INTERFACES}=:080650:` (colon-separated interface classes)
- `ENV{ID_VENDOR_FROM_DATABASE}`, `ENV{ID_MODEL_FROM_DATABASE}` (from `usb.ids` via `hwdb`)

There is **no `udevadm power` subcommand**; "udev power control" is
done by writing rules that write to the sysfs files above. Example
rule (in `/etc/udev/rules.d/50-usb-power.rules`):

```
# Disable autosuspend on USB HID devices (some keyboards drop keys)
ACTION=="add", SUBSYSTEM=="usb", ATTR{bInterfaceClass}=="03", \
        TEST=="power/control", ATTR{power/control}="on"
```

To trigger an autosuspend immediately for a class of devices:

```
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="1234", \
        TEST=="power/control", ATTR{power/control}="auto", \
        ATTR{power/autosuspend_delay_ms}="1000"
```

`udevadm trigger --action=add --subsystem-match=usb` re-runs rules for
existing devices.

---

## 2. Bluetooth Domain

### 2.1 Bluetooth adapter sysfs

Bluetooth adapters register under the `bluetooth` device class:

```
/sys/class/bluetooth/hci<N>/
```

Kernel source: `net/bluetooth/hci_sysfs.c`. The class itself only
exposes one device attribute:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/class/bluetooth/hci<N>/reset` | WO | any data | Triggers the vendor `reset` callback (`hdev->reset`). Kernel 6.13+. |

Plus the standard device attributes from the parent (often a USB
interface or platform device): `uevent`, `subsystem`, `driver` (if a
bus driver like `btusb` is bound), `power/` (runtime PM, see section
1.2 if the adapter is on USB), `modalias`.

Other adapter attributes (address, type, manufacturer, version,
features) are **not in sysfs**. They live in:

- **debugfs** at `/sys/kernel/debug/bluetooth/hci<N>/` (read-only, requires root + `CONFIG_DEBUG_FS`).
- The kernel management socket (`mgmt` socket, see 2.5).
- BlueZ D-Bus (see 2.4).

#### 2.1.1 Bluetooth debugfs entries

From `net/bluetooth/hci_debugfs.c`:

| Path | R/W | Format |
|---|---|---|
| `/sys/kernel/debug/bluetooth/hci<N>/features` | RO | 8-byte feature bitmask, hex |
| `/sys/kernel/debug/bluetooth/hci<N>/manufacturer` | RO | u16 |
| `/sys/kernel/debug/bluetooth/hci<N>/hci_version` | RO | u8 |
| `/sys/kernel/debug/bluetooth/hci<N>/hci_revision` | RO | u16 |
| `/sys/kernel/debug/bluetooth/hci<N>/hardware_error` | RO | u8 |
| `/sys/kernel/debug/bluetooth/hci<N>/device_id` | RO | hex |
| `/sys/kernel/debug/bluetooth/hci<N>/device_list` | RO | |
| `/sys/kernel/debug/bluetooth/hci<N>/blacklist` | RO | BD_ADDR list |
| `/sys/kernel/debug/bluetooth/hci<N>/blocked_keys` | RO | |
| `/sys/kernel/debug/bluetooth/hci<N>/uuids` | RO | |
| `/sys/kernel/debug/bluetooth/hci<N>/remote_oob` | RO | |
| `/sys/kernel/debug/bluetooth/hci<N>/conn_info_min_age` | RW | ms |
| `/sys/kernel/debug/bluetooth/hci<N>/conn_info_max_age` | RW | ms |
| `/sys/kernel/debug/bluetooth/hci<N>/use_debug_keys` | RO | bool |
| `/sys/kernel/debug/bluetooth/hci<N>/sc_only_mode` | RO | bool |
| `/sys/kernel/debug/bluetooth/hci<N>/hardware_info` | RO | string |
| `/sys/kernel/debug/bluetooth/hci<N>/firmware_info` | RO | string |
| `/sys/kernel/debug/bluetooth/hci<N>/dut_mode` | RW | bool (Device Under Test mode) |
| `/sys/kernel/debug/bluetooth/hci<N>/inquiry_cache` | RO | BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/link_keys` | RO | BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/dev_class` | RO | BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/voice_setting` | RO | BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/auto_accept_delay` | RW | ms, BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/idle_timeout` | RW | ms, BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/sniff_min_interval` | RW | BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/sniff_max_interval` | RW | BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/min_encrypt_key_size` | RW | BR/EDR only |
| `/sys/kernel/debug/bluetooth/hci<N>/identity` | RO | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/rpa_timeout` | RW | s, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/random_address` | RO | BD_ADDR, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/static_address` | RO | BD_ADDR, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/force_static_address` | RW | bool, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/white_list_size` | RO | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/white_list` | RO | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/resolv_list_size` | RO | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/resolv_list` | RO | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/identity_resolving_keys` | RO | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/long_term_keys` | RO | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/conn_min_interval` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/conn_max_interval` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/conn_latency` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/supervision_timeout` | RW | ms, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/adv_channel_map` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/adv_min_interval` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/adv_max_interval` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/discov_interleaved_timeout` | RW | s, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/min_key_size` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/max_key_size` | RW | LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/auth_payload_timeout` | RW | ms, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/force_no_mitm` | RW | bool, LE only |
| `/sys/kernel/debug/bluetooth/hci<N>/quirk_*` | RW | bool, several |

Connection-scoped entries live under
`/sys/kernel/debug/bluetooth/hci<N>/<conn_name>/`.

### 2.2 Bluetooth rfkill

Bluetooth adapters typically register an rfkill device so the
airplane-mode / kill-switch subsystem can soft-block them. See section
3 of this document for the full rfkill ABI; the Bluetooth-specific
notes are:

| Path | Notes |
|---|---|
| `/sys/class/rfkill/rfkill<N>/name` | Adapter name, e.g. `hci0` or `tpacpi_bluetooth_sw`. |
| `/sys/class/rfkill/rfkill<N>/type` | `bluetooth` for an HCI adapter. |
| `/sys/class/rfkill/rfkill<N>/soft` | RW `0` / `1`. `1` = software-blocked (BT radio off). |
| `/sys/class/rfkill/rfkill<N>/hard` | RO `0` / `1`. `1` = hardware-blocked (physical switch). |
| `/sys/class/rfkill/rfkill<N>/persistent` | RO `0` / `1`. `1` if the soft-block state survives reboot / module reload. |
| `/sys/class/rfkill/rfkill<N>/state` | RW `0` / `1` / `2`. Legacy combined state. Prefer `soft` + `hard`. |

To soft-block Bluetooth from a shell:

```
echo 1 > /sys/class/rfkill/rfkill0/soft
```

The `/dev/rfkill` character device (see 3.2) is the preferred
programmatic interface.

### 2.3 Bluetooth over USB

If the adapter is USB-connected (most laptops), the `btusb` driver
binds to a USB interface. The USB power-management paths in section 1
apply directly to the adapter:

```
/sys/bus/usb/devices/<bt-usb-iface>/../power/control              # autosuspend
/sys/bus/usb/devices/<bt-usb-iface>/../power/autosuspend_delay_ms # delay
/sys/bus/usb/devices/<bt-usb-iface>/../power/persist
```

btusb module parameters (from `drivers/bluetooth/btusb.c`):

| Path | Format | Notes |
|---|---|---|
| `/sys/module/btusb/parameters/disable_scofix` | bool | Disable fixup of wrong SCO buffer size. |
| `/sys/module/btusb/parameters/force_scofix` | bool | Force fixup of wrong SCO buffer size. |
| `/sys/module/btusb/parameters/enable_autosuspend` | bool | Default = `CONFIG_BT_HCIBTUSB_AUTOSUSPEND`. When true, `usb_enable_autosuspend()` is called at probe. |
| `/sys/module/btusb/parameters/reset` | bool | Default true. Send `HCI_Reset` on init. |

Other Bluetooth driver module parameters worth knowing:

| Driver | Path | Format | Notes |
|---|---|---|---|
| `btmtksdio` | `/sys/module/btmtksdio/parameters/enable_autosuspend` | bool | SDIO MT7663/7921. |
| `hci_bcm` | `/sys/module/hci_bcm/parameters/irq_polarity` | int (read-only) | 0 = active-high, 1 = active-low. |
| `hci_bcsp` | `/sys/module/hci_bcsp/parameters/txcrc` | bool | |
| `hci_bcsp` | `/sys/module/hci_bcsp/parameters/hciextn` | bool | |
| `hci_vhci` | `/sys/module/hci_vhci/parameters/amp` | bool | Create AMP controller. |

### 2.4 BlueZ D-Bus API

BlueZ (the user-space Bluetooth stack) exposes adapter and device
control over D-Bus. Service name `org.bluez`. The relevant object
paths and interfaces:

| Object path | Interface | Power-relevant properties |
|---|---|---|
| `/org/bluez/hci<N>` | `org.bluez.Adapter1` | `Powered` (bool, RW), `Discoverable`, `Pairable`, `Address`, `AddressType`, `Name`, `Alias`, `Class`, `Manufacturer`, `Version`, `SupportedModes`, `ActiveModes`. |
| `/org/bluez/hci<N>` | `org.freedesktop.DBus.Properties` | `Get` / `Set` / `GetAll` / `PropertiesChanged` signal. |
| `/org/bluez/hci<N>/dev_XX_XX_XX_XX_XX_XX` | `org.bluez.Device1` | `Connected`, `Paired`, `Trusted`, `Blocked`, `RSSI`, `Address`, `Name`, `Adapter`. |
| `/` | `org.bluez.AgentManager1` | Register pairing agent. |
| `/` | `org.bluez.ProfileManager1` | Register a profile. |

Adapter power on / off via D-Bus:

```
gdbus call -y -t ms -d org.bluez \
    -o /org/bluez/hci0 \
    -m org.freedesktop.DBus.Properties.Set \
    string:org.bluez.Adapter1 variant:string:Powered \
    "<true>"
```

`bluetoothctl` is the interactive front-end (`power on`, `power off`,
`show`, `devices`, `connect <addr>`).

Under the hood, `Powered=true` causes BlueZ to send `MGMT_OP_SET_POWERED`
to the kernel management socket (see 2.5), which calls `HCIDEVUP` /
`HCIDEVDOWN` on the HCI socket (see 2.6).

### 2.5 Bluetooth management socket (mgmt)

The preferred programmatic interface for controlling adapters without
going through BlueZ. Defined in `include/net/bluetooth/mgmt.h`.

Open a raw HCI control socket:

```c
int fd = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
struct sockaddr_hci addr = { .hci_family = AF_BLUETOOTH,
                             .hci_dev = HCI_DEV_NONE,
                             .hci_channel = HCI_CHANNEL_CONTROL };
bind(fd, (struct sockaddr *)&addr, sizeof(addr));
```

Send commands as `struct mgmt_hdr` + command payload; receive events
as `mgmt_hdr` + event payload. Key commands:

| Command | Opcode | Payload | Effect |
|---|---|---|---|
| `MGMT_OP_READ_VERSION` | 0x0001 | — | Read mgmt protocol version. |
| `MGMT_OP_READ_COMMANDS` | 0x0002 | — | List supported commands / events. |
| `MGMT_OP_READ_INDEX_LIST` | 0x0003 | — | List adapter indices. |
| `MGMT_OP_READ_INFO` | 0x0004 | adapter index | Returns `bdaddr`, `version`, `manufacturer`, `supported_settings`, `current_settings`, `dev_class`, `name`, `short_name`. |
| `MGMT_OP_SET_POWERED` | 0x0005 | adapter index + `struct mgmt_mode { u8 val; }` | `val=1` powers on, `val=0` powers off. |
| `MGMT_OP_SET_DISCOVERABLE` | 0x0006 | index + mode + timeout | |
| `MGMT_OP_SET_CONNECTABLE` | 0x0007 | index + mode | |
| `MGMT_OP_SET_BONDABLE` | 0x0009 | index + mode | |
| `MGMT_OP_SET_LE` | 0x000D | index + mode | Enable LE. |
| `MGMT_OP_SET_BREDR` | 0x000B (set BR/EDR) | | |
| `MGMT_OP_SET_ADVERTISING` | (in `MGMT_SETTING_ADVERTISING`) | | LE advertising. |
| `MGMT_OP_LOAD_LINK_KEYS` | 0x0012 | | |
| `MGMT_OP_DISCONNECT` | 0x0014 | | |
| `MGMT_OP_GET_CONNECTIONS` | 0x0015 | | |

`current_settings` is a `__le32` bitmask; relevant bits:

| Bit | Setting |
|---|---|
| 0 | `POWERED` |
| 1 | `CONNECTABLE` |
| 2 | `FAST_CONNECTABLE` |
| 3 | `DISCOVERABLE` |
| 4 | `BONDABLE` |
| 5 | `LINK_SECURITY` |
| 6 | `SSP` |
| 7 | `BREDR` |
| 8 | `HS` |
| 9 | `LE` |
| 10 | `ADVERTISING` |
| 11 | `SECURE_CONN` |

This is the same bitmask BlueZ uses for the `Adapter1.CurrentSettings`
D-Bus property.

### 2.6 Bluetooth HCI socket ioctls

The legacy (pre-mgmt) interface for `hciconfig`. Defined in
`include/net/bluetooth/hci_sock.h`.

Open:

```c
int fd = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
struct sockaddr_hci addr = { .hci_family = AF_BLUETOOTH,
                             .hci_dev = 0,        /* hci0 */
                             .hci_channel = HCI_CHANNEL_RAW };
bind(fd, (struct sockaddr *)&addr, sizeof(addr));
```

IOCTLs (the `int` argument is the adapter index, e.g. `0` for `hci0`):

| Ioctl | Magic | Number | Effect |
|---|---|---|---|
| `HCIDEVUP` | `'H'` | 201 | Bring adapter up. Equivalent to `hciconfig hci0 up`. Under the hood: opens the transport, sends `HCI_Reset`, runs `setup()`. |
| `HCIDEVDOWN` | `'H'` | 202 | Bring adapter down. `hciconfig hci0 down`. |
| `HCIDEVRESET` | `'H'` | 203 | Reset the adapter (send `HCI_Reset`). |
| `HCIDEVRESTAT` | `'H'` | 204 | Reset statistics counters. |
| `HCIGETDEVLIST` | `'H'` | 210 | List adapters (`struct hci_dev_list_req`). |
| `HCIGETDEVINFO` | `'H'` | 211 | Get adapter info (`struct hci_dev_info` with `bdaddr`, `flags`, `type`, `features`, `pkt_type`, `link_policy`, `link_mode`, `acl_mtu`, `acl_pkts`, `sco_mtu`, `sco_pkts`, `stat`). |
| `HCIGETCONNLIST` | `'H'` | 212 | List active connections. |
| `HCIGETCONNINFO` | `'H'` | 213 | Get info for a specific connection. |
| `HCIGETAUTHINFO` | `'H'` | 215 | |
| `HCISETRAW` | `'H'` | 220 | Put adapter in raw mode. |
| `HCISETSCAN` | `'H'` | 221 | Set scan mode (inquiry / page scan). |
| `HCISETAUTH` | `'H'` | 222 | Enable / disable authentication. |
| `HCISETENCRYPT` | `'H'` | 223 | Enable / disable encryption. |
| `HCISETPTYPE` | `'H'` | 224 | Packet types. |
| `HCISETLINKPOL` | `'H'` | 225 | Link policy. |
| `HCISETLINKMODE` | `'H'` | 226 | Link mode. |
| `HCISETACLMTU` | `'H'` | 227 | ACL MTU. |
| `HCISETSCOMTU` | `'H'` | 228 | SCO MTU. |
| `HCIBLOCKADDR` | `'H'` | 230 | Block a BD_ADDR. |
| `HCIUNBLOCKADDR` | `'H'` | 231 | Unblock. |
| `HCIINQUIRY` | `'H'` | 240 | Start inquiry. |

`hciconfig hci0 up` under the hood opens a raw HCI socket, calls
`ioctl(fd, HCIDEVUP, 0)`. Modern tools (`bluetoothctl`, `btmgmt`) use
the mgmt socket instead — `hciconfig` is deprecated.

Socket channels:

| Channel | Constant | Use |
|---|---|---|
| 0 | `HCI_CHANNEL_RAW` | Direct access to the controller (raw HCI commands). Used by `hciconfig`, vendor tools. |
| 1 | `HCI_CHANNEL_USER` | Like RAW but exclusive — kernel stops processing events itself. Used by BlueZ. |
| 2 | `HCI_CHANNEL_MONITOR` | Read-only tap of all HCI traffic. `btmon` uses this. |
| 3 | `HCI_CHANNEL_CONTROL` | Management socket. |
| 4 | `HCI_CHANNEL_LOGGING` | Push log records into the `btmon` stream. |

### 2.7 Bluetooth module parameters

The `bluetooth` core module has no writable module parameters in
recent kernels. What's tunable lives per-driver:

```
/sys/module/bluetooth/parameters/       # inspect; recent kernels: no tunables
/sys/module/btusb/parameters/           # see 2.3
/sys/module/btintel/parameters/         # none
/sys/module/btbcm/parameters/           # none
```

`/sys/module/bluetooth/` itself has the standard `version`,
`srcversion`, `initstate`, `refcnt`, `holders/` entries.

### 2.8 Vendor quirks and ACPI Bluetooth

Many laptops wire the Bluetooth radio through ACPI or vendor-specific
EC commands instead of plain rfkill:

| Driver | Path | Notes |
|---|---|---|
| `thinkpad_acpi` | `/proc/acpi/ibm/bluetooth` | `echo enable\|disable` to toggle the ThinkPad internal BT CDC slot. Status: `cat /proc/acpi/ibm/bluetooth`. State persists in NVRAM. The `bluetooth_enable` sysfs attribute is deprecated; use rfkill (`tpacpi_bluetooth_sw`) instead. |
| `ideapad-laptop` | `/sys/bus/platform/drivers/ideapad_acpi/.../bluetooth_power` | bool RW. |
| `dell-rbtn` | rfkill device `dell-rbtn` | Dell radio button driver. |
| `hp-wmi` | rfkill `hp-wifi` / `hp-bluetooth` / `hp-wwan` | HP WMI radio switches. |
| `asus-wmi` | `wlan_enable`, `bluetooth_enable`, `wimax_enable`, `wwan_enable` attributes under the platform device. | |

These all eventually show up as rfkill devices; prefer the rfkill
interface (`/sys/class/rfkill/` or `/dev/rfkill`) over the
vendor-specific procfs / sysfs files.

---

## 3. Wireless Domain (Wi-Fi / cfg80211 / nl80211)

### 3.1 Wireless phys and interfaces

The cfg80211 subsystem exposes one `wiphy` (wireless PHY) per radio:

```
/sys/class/ieee80211/phy<N>/
```

Kernel source: `net/wireless/sysfs.c`. Attributes:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/class/ieee80211/phy<N>/index` | RO | decimal | Wiphy index (0-based). |
| `/sys/class/ieee80211/phy<N>/name` | RO | `phy0`, `phy1`, ... | Wiphy name. |
| `/sys/class/ieee80211/phy<N>/macaddress` | RO | `xx:xx:xx:xx:xx:xx` | Permanent MAC (`wiphy.perm_addr`). |
| `/sys/class/ieee80211/phy<N>/address_mask` | RO | `xx:xx:xx:xx:xx:xx` | Bitmask of variable address bits. |
| `/sys/class/ieee80211/phy<N>/addresses` | RO | newline-separated MAC list | All available MACs if `wiphy.addresses` is set; otherwise just `macaddress`. |

The netdev corresponding to a wiphy lives at `/sys/class/net/<iface>/`
with `phy80211` -> symlink to `/sys/class/ieee80211/phy<N>/`. Standard
netdev runtime PM attributes (`power/control`, `power/autosuspend_delay_ms`,
etc., section 1.2) apply because the netdev is a `struct device`.

Wireless LEDs (when the driver exposes them) live at
`/sys/class/leds/<name>/` and the driver may register a
`/sys/class/ieee80211/phy<N>/device/leds/` symlink (kernel-dependent;
mac80211 manages `tpt-trig` throughput-trigger LEDs).

### 3.2 nl80211 / cfg80211

nl80211 is the netlink-based configuration interface for cfg80211.
`iw` is the user-space tool. The kernel side lives in
`net/wireless/nl80211.c`. Header: `include/uapi/linux/nl80211.h`.

Netlink family: `nl80211`, family ID obtained via
`genl_ctrl_resolve(sock, "nl80211")`. Multicast groups: `config`,
`scan`, `mlme`, `regulatory`, `vendor`, `nbconn`, `testmode`.

Key commands and the attributes they consume:

| Command | Purpose | Key attributes |
|---|---|---|
| `NL80211_CMD_GET_WIPHY` / `NL80211_CMD_SET_WIPHY` | Get / set wiphy params. | `NL80211_ATTR_WIPHY` (index), `NL80211_ATTR_IFINDEX`, `NL80211_ATTR_WIPHY_NAME`, `NL80211_ATTR_WIPHY_TXQ_PARAMS`, `NL80211_ATTR_WIPHY_FREQ`, `NL80211_ATTR_WIPHY_FREQ_OFFSET`, `NL80211_ATTR_WIPHY_RETRY_SHORT`, `NL80211_ATTR_WIPHY_RETRY_LONG`, `NL80211_ATTR_WIPHY_FRAG_THRESHOLD`, `NL80211_ATTR_WIPHY_RTS_THRESHOLD`, `NL80211_ATTR_WIPHY_TX_POWER_SETTING`, `NL80211_ATTR_WIPHY_TX_POWER_LEVEL`. |
| `NL80211_CMD_GET_INTERFACE` / `NL80211_CMD_SET_INTERFACE` / `NL80211_CMD_NEW_INTERFACE` / `NL80211_CMD_DEL_INTERFACE` | Manage virtual interfaces. | `NL80211_ATTR_IFINDEX`, `NL80211_ATTR_IFTYPE`, `NL80211_ATTR_IFNAME`, `NL80211_ATTR_WIPHY`. |
| `NL80211_CMD_SET_CHANNEL` | Set channel for an interface or wiphy. | `NL80211_ATTR_WIPHY_FREQ`, `NL80211_ATTR_WIPHY_CHANNEL_TYPE`, `NL80211_ATTR_CHANNEL_WIDTH`, `NL80211_ATTR_CENTER_FREQ1`, `NL80211_ATTR_CENTER_FREQ2`. |
| `NL80211_CMD_SET_REG` | Set regulatory domain (kernel internal / CRDA). | `NL80211_ATTR_REG_ALPHA2`, `NL80211_ATTR_REG_RULES`. |
| `NL80211_CMD_REQ_SET_REG` | Userspace request to set regdomain. | `NL80211_ATTR_REG_ALPHA2` (ISO 3166-1 alpha-2). |
| `NL80211_CMD_GET_REG` | Get current regulatory domain. | Returns rules per frequency range. |
| `NL80211_CMD_SET_TX_BITRATE_MASK` | Per-interface TX bitrate mask. | `NL80211_ATTR_TX_RATES` (nested per-band). |
| `NL80211_CMD_SET_POWER_SAVE` / `NL80211_CMD_GET_POWER_SAVE` | Wi-Fi power save. | `NL80211_ATTR_PS_STATE` (`NL80211_PS_DISABLED` / `NL80211_PS_ENABLED`). |
| `NL80211_CMD_GET_SCAN` / `NL80211_CMD_TRIGGER_SCAN` / `NL80211_CMD_NEW_SCAN_RESULTS` / `NL80211_CMD_SCAN_ABORTED` | Scan management. | |
| `NL80211_CMD_CONNECT` / `NL80211_CMD_DISCONNECT` / `NL80211_CMD_ROAM` | Connection management (managed mode). | |
| `NL80211_CMD_START_AP` / `NL80211_CMD_STOP_AP` | AP mode. | `NL80211_ATTR_BEACON_HEAD`, `NL80211_ATTR_BEACON_TAIL`, `NL80211_ATTR_BEACON_INTERVAL`, `NL80211_ATTR_DTIM_PERIOD`, `NL80211_ATTR_SSID`. |
| `NL80211_CMD_JOIN_IBSS` / `NL80211_CMD_LEAVE_IBSS` | Ad-hoc mode. | |
| `NL80211_CMD_JOIN_MESH` / `NL80211_CMD_LEAVE_MESH` | 802.11s mesh. | |
| `NL80211_CMD_SET_WIPHY_NETNS` | Move wiphy to another network namespace. | |
| `NL80211_CMD_GET_SURVEY` / `NL80211_CMD_NEW_SURVEY_RESULTS` | Channel survey (busy / noise / TX time). | `NL80211_SURVEY_INFO_FREQUENCY`, `NL80211_SURVEY_INFO_NOISE`, `NL80211_SURVEY_INFO_CHANNEL_TIME`, `NL80211_SURVEY_INFO_CHANNEL_TIME_BUSY`, `NL80211_SURVEY_INFO_IN_USE`. |
| `NL80211_CMD_GET_WOWLAN` / `NL80211_CMD_SET_WOWLAN` | Wake-on-WLAN configuration. | |

Interface types (`enum nl80211_iftype`):

| Value | Type |
|---|---|
| 0 | `NL80211_IFTYPE_UNSPECIFIED` |
| 1 | `NL80211_IFTYPE_ADHOC` (ibss) |
| 2 | `NL80211_IFTYPE_STATION` (managed) |
| 3 | `NL80211_IFTYPE_AP` |
| 4 | `NL80211_IFTYPE_AP_VLAN` |
| 5 | `NL80211_IFTYPE_WDS` |
| 6 | `NL80211_IFTYPE_MONITOR` |
| 7 | `NL80211_IFTYPE_MESH_POINT` |
| 8 | `NL80211_IFTYPE_P2P_CLIENT` |
| 9 | `NL80211_IFTYPE_P2P_GO` |
| 10 | `NL80211_IFTYPE_P2P_DEVICE` (no netdev) |
| 11 | `NL80211_IFTYPE_OCB` (Outside Context of BSS) |
| 12 | `NL80211_IFTYPE_NAN` (no netdev) |

TX power setting (`enum nl80211_tx_power_setting`):

| Value | Meaning |
|---|---|
| 0 | `NL80211_TX_POWER_AUTOMATIC` — driver picks. |
| 1 | `NL80211_TX_POWER_LIMITED` — limit to the given mBm. |
| 2 | `NL80211_TX_POWER_FIXED` — fix to exactly the given mBm. |

`NL80211_ATTR_WIPHY_TX_POWER_LEVEL` is signed mBm (100 * dBm). To set
fixed 10 dBm: `setting=NL80211_TX_POWER_FIXED`, `level=1000`. To set
auto: `setting=NL80211_TX_POWER_AUTOMATIC`, no level.

Per-frequency TX power limit: `NL80211_FREQUENCY_ATTR_MAX_TX_POWER`
(in mBm) returned by `NL80211_CMD_GET_WIPHY` for each frequency.

### 3.3 iw command mapping

The `iw` tool is a thin wrapper around nl80211. Common Zenctl-relevant
commands and their nl80211 equivalents:

| iw command | nl80211 call |
|---|---|
| `iw list` | `NL80211_CMD_GET_WIPHY` dump. |
| `iw phy phy0 info` | `NL80211_CMD_GET_WIPHY` for one wiphy. |
| `iw dev` | `NL80211_CMD_GET_INTERFACE` dump. |
| `iw dev wlan0 set type managed` | `NL80211_CMD_SET_INTERFACE` with `NL80211_ATTR_IFTYPE=2`. |
| `iw dev wlan0 set type ap` | `NL80211_ATTR_IFTYPE=3`. |
| `iw dev wlan0 set type ibss` | `NL80211_ATTR_IFTYPE=1`. |
| `iw dev wlan0 set type monitor` | `NL80211_ATTR_IFTYPE=6`. |
| `iw dev wlan0 set type mesh` | `NL80211_ATTR_IFTYPE=7`. |
| `iw dev wlan0 set txpower fixed 2000` | `NL80211_CMD_SET_WIPHY` with `WIPHY_TX_POWER_SETTING=FIXED`, `WIPHY_TX_POWER_LEVEL=2000` (mBm = 20 dBm). |
| `iw dev wlan0 set txpower auto` | `WIPHY_TX_POWER_SETTING=AUTOMATIC`. |
| `iw dev wlan0 set txpower limit 1500` | `WIPHY_TX_POWER_SETTING=LIMITED`, `WIPHY_TX_POWER_LEVEL=1500` (15 dBm cap). |
| `iw dev wlan0 set power_save on` | `NL80211_CMD_SET_POWER_SAVE` with `NL80211_ATTR_PS_STATE=ENABLED`. |
| `iw dev wlan0 set power_save off` | `PS_STATE=DISABLED`. |
| `iw dev wlan0 set bitrates legacy-2.4 12 18 24` | `NL80211_CMD_SET_TX_BITRATE_MASK` with a nested `NL80211_ATTR_TX_RATES` containing `NL80211_BAND_2GHZ` and per-legacy-rate enable bits. |
| `iw reg set DE` | `NL80211_CMD_REQ_SET_REG` with `NL80211_ATTR_REG_ALPHA2="DE"`. |
| `iw reg get` | `NL80211_CMD_GET_REG`. |
| `iw phy phy0 set netns $PID` | `NL80211_CMD_SET_WIPHY_NETNS`. |
| `iw dev wlan0 link` | `NL80211_CMD_GET_STATION` for the connected BSSID. |
| `iw dev wlan0 survey dump` | `NL80211_CMD_GET_SURVEY` dump. |
| `iw dev wlan0 scan` | `NL80211_CMD_TRIGGER_SCAN` then wait for `NL80211_CMD_NEW_SCAN_RESULTS`. |
| `iw phy phy0 set antenna 1 1` | `NL80211_CMD_SET_WIPHY` with `NL80211_ATTR_WIPHY_ANTENNA_TX` / `_RX`. |
| `iw phy phy0 set rts 512` | `NL80211_CMD_SET_WIPHY` with `NL80211_ATTR_WIPHY_RTS_THRESHOLD=512`. |
| `iw phy phy0 set frag 512` | `NL80211_CMD_SET_WIPHY` with `NL80211_ATTR_WIPHY_FRAG_THRESHOLD=512`. |
| `iw phy phy0 set retry short 7 long 4` | `NL80211_CMD_SET_WIPHY` with `NL80211_ATTR_WIPHY_RETRY_SHORT=7`, `_LONG=4`. |

### 3.4 Regulatory domain

Kernel source: `net/wireless/reg.c`, `Documentation/networking/regulatory.rst`.

The regdomain database ships as `/lib/firmware/regulatory.db` (with a
signature file `regulatory.db.p7s`). The kernel loads it on demand
via the firmware loader.

Setting the regdomain:

```
iw reg set <ISO-alpha2>      # e.g. iw reg set DE
```

Under the hood: `iw` sends `NL80211_CMD_REQ_SET_REG` with
`NL80211_ATTR_REG_ALPHA2`. The kernel records the request, looks up
the rules in `regulatory.db`, applies them. If the kernel cannot find
the alpha2, it sends a uevent (`COUNTRY=<alpha2>`) which the legacy
`crda` userspace agent listens for.

Reading the current regdomain:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `iw reg get` | — | formatted output | Source of truth. |
| `/sys/class/regulatory/` | — | dir | Per-phy regulatory hints in older kernels; now mostly informational. |

Self-managed regulatory drivers (`wiphy.regulatory_flags |= REGULATORY_WIPHY_SELF_MANAGED`) bypass the global database and set their own rules via `NL80211_CMD_SET_REG` with a `NL80211_ATTR_REG_ALPHA2` plus a full rule set.

### 3.5 Wireless power management

Wi-Fi has two layers of PM:

1. **Per-netdev runtime PM** (PCI/USB device-level, sections 1.2 and 5):

   ```
   /sys/class/net/<iface>/power/control
   /sys/class/net/<iface>/power/autosuspend_delay_ms
   /sys/class/net/<iface>/power/runtime_status
   ```

   The netdev's parent (the PCI or USB device) has the same files; the
   netdev's `power/` is its own runtime PM domain.

2. **802.11 power save** (Wi-Fi protocol-level, nl80211):

   `iw dev <iface> set power_save on` puts the interface in 802.11 PS
   mode: the radio sleeps between beacons, wakes at the DTIM interval
   to receive buffered multicast / broadcasts. Trades latency for
   power. Not all drivers support it; check `iw phy phy0 info` for
   `interface combinations` and `supported commands: ... set_power_save`.

### 3.6 Wireless bitrate / TX rate

`iw dev <iface> set bitrates` sets a mask of allowed rates. Format:

```
iw dev wlan0 set bitrates legacy-2.4 6 12 24 ht-mcs-2.4 0 1 2 vht-mcs-5 1:0 2:0
```

- `legacy-2.4` / `legacy-5` — list of legacy rates in Mbps.
- `ht-mcs-2.4` / `ht-mcs-5` — list of HT MCS indexes (0–76).
- `vht-mcs-5` — list of `N:M` pairs (N = NSS, M = MCS).
- `he-mcs-` / `eht-mcs-` — HE and EHT equivalents.
- `clear` to reset.

Current rate: `iw dev <iface> link` (in managed mode, shows the
`tx bitrate: X.X MBit/s MCS X ...`). `iw dev <iface> station dump`
shows per-station rates in AP mode.

### 3.7 Wireless extensions (legacy)

Kernel source: `include/uapi/linux/wireless.h`, `net/core/wireless.c`.

The ioctl-based wireless extensions predate nl80211. They are
deprecated but still present for `iwconfig` and `/proc/net/wireless`.

| Path | Format |
|---|---|
| `/proc/net/wireless` | Per-interface stats: `Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE`, columns `status`, `link`, `level`, `noise`, `nwid`, `crypt`, `frag`, `retry`, `misc`, `missed` (all but `status` are integers; `link/level/noise` may be dBm if `IW_QUAL_DBM` flag set). |

Wireless-extensions ioctls (`SIOCSIW*` / `SIOCGIW*`):

| Ioctl | Magic | Number | Purpose |
|---|---|---|---|
| `SIOCSIWMODE` / `SIOCGIWMODE` | `0x89` | 0x06 / 0x07 | Set / get mode. Values: `IW_MODE_AUTO`, `IW_MODE_ADHOC`, `IW_MODE_INFRA`, `IW_MODE_MASTER`, `IW_MODE_REPEAT`, `IW_MODE_SECOND`, `IW_MODE_MONITOR`, `IW_MODE_MESH`. |
| `SIOCSIWFREQ` / `SIOCGIWFREQ` | `0x89` | 0x04 / 0x05 | Set / get channel / frequency. |
| `SIOCSIWESSID` / `SIOCGIWESSID` | `0x89` | 0x1A / 0x1B | Set / get SSID. |
| `SIOCSIWTXPOW` / `SIOCGIWTXPOW` | `0x89` | 0x2E / 0x2F | Set / get TX power. Flags: `IW_TXPOW_DBM`, `IW_TXPOW_MWATT`, `IW_TXPOW_RELATIVE`, `IW_TXPOW_RANGE`. |
| `SIOCSIWPOWER` / `SIOCGIWPOWER` | `0x89` | 0x30 / 0x31 | Set / get power management. Flags: `IW_POWER_PERIOD`, `IW_POWER_TIMEOUT`, `IW_POWER_UNICAST_R`, `IW_POWER_ALL_R`, `IW_POWER_FORCE_S`, `IW_POWER_REPEATER`, `IW_POWER_ON`, `IW_POWER_MIN`, `IW_POWER_MAX`. |
| `SIOCSIWRETRY` / `SIOCGIWRETRY` | `0x89` | 0x32 / 0x33 | Set / get retry limits. |
| `SIOCSIWSENS` / `SIOCGIWSENS` | `0x89` | 0x08 / 0x09 | Sensitivity threshold. |
| `SIOCSIWRATE` / `SIOCGIWRATE` | `0x89` | 0x20 / 0x21 | Default bit rate. |
| `SIOCGIWSTATS` | `0x89` | 0x0F | Get `/proc/net/wireless`-equivalent stats. |
| `SIOCGIWRANGE` | `0x89` | 0x0B | Get the full `iw_range` (supported rates, channels, encodings, frequencies, PM capabilities, etc.). |
| `SIOCSIWAP` / `SIOCGIWAP` | `0x89` | 0x14 / 0x15 | Set / get AP MAC. |
| `SIOCSIWSCAN` / `SIOCGIWSCAN` | `0x89` | 0x18 / 0x19 | Trigger / fetch scan. |

`iwconfig` uses these. **Prefer `iw` / nl80211** for new code. WEXT
is frozen; new drivers should not implement it.

### 3.8 Wireless coexistence (BT/WiFi)

2.4 GHz Bluetooth and Wi-Fi share spectrum. Most combo chips
(Intel Wireless-AC, Realtek RTL8822, MediaTek MT7921) handle
coexistence in firmware. Driver-level knobs (if exposed):

- `iwpriv wlan0 setBTCfg <args>` — per-driver private ioctls (e.g.
  Intel `iwlmvm`).
- Debugfs under `/sys/kernel/debug/ieee80211/phy<N>/`:
  `/sys/kernel/debug/ieee80211/phy0/iwlwifi/iwlmvm/bt_stats`
  (Intel-specific).
- Module params, e.g. `/sys/module/iwlmvm/parameters/power_scheme`
  (1=CAM, 2=balanced, 3=power-save).

There is **no generic** nl80211 / sysfs coex knob; it's per-driver.

### 3.9 rfkill

rfkill is the unified radio-kill subsystem. It covers Wi-Fi, BT, WWAN,
UWB, WiMAX, GPS, FM, NFC.

Kernel source: `net/rfkill/core.c`, `include/uapi/linux/rfkill.h`,
`include/linux/rfkill.h`. ABI: `Documentation/ABI/stable/sysfs-class-rfkill`.

#### 3.9.1 sysfs interface

```
/sys/class/rfkill/rfkill<N>/
```

| Path | R/W | Format | Notes |
|---|---|---|---|
| `name` | RO | string | Driver-supplied name (e.g. `phy0`, `hci0`, `tpacpi_bluetooth_sw`). |
| `type` | RO | `wlan` / `bluetooth` / `wwan` / `uwb` / `wimax` / `gps` / `fm` / `nfc` | From `enum rfkill_type`. |
| `persistent` | RO | `0` / `1` | `1` if soft-block state is restored from non-volatile storage at boot. |
| `state` | RW | `0` / `1` / `2` | Legacy combined state. `0` = soft-blocked, `1` = unblocked, `2` = hard-blocked. **Prefer `soft` + `hard`.** |
| `hard` | RO | `0` / `1` | Hard-block state. `1` = forced off by hardware (physical switch). |
| `soft` | RW | `0` / `1` | Soft-block state. `0` = unblocked, `1` = blocked (radio off). |
| `index` | RO | decimal | rfkill index. |
| `uevent` | RO | | Standard `uevent` attribute. |

Deprecated and removed (see `Documentation/ABI/removed/sysfs-class-rfkill`):
`claim` — no longer present.

#### 3.9.2 /dev/rfkill

The character device `/dev/rfkill` (mode `0666` or `0660` group
`rfkill`, depending on udev) is the preferred programmatic interface.
It carries `struct rfkill_event` (or `struct rfkill_event_ext` for
extended events):

```c
struct rfkill_event {
    __u32 idx;          /* rfkill index */
    __u8  type;         /* enum rfkill_type */
    __u8  op;           /* enum rfkill_operation: ADD / DEL / CHANGE / CHANGE_ALL */
    __u8  soft;         /* new soft state */
    __u8  hard;         /* hard state */
} __attribute__((packed));

struct rfkill_event_ext {
    __u32 idx;
    __u8  type;
    __u8  op;
    __u8  soft;
    __u8  hard;
    __u8  hard_block_reasons;  /* bitmask: 1<<0 = SIGNAL, 1<<1 = NOT_OWNER */
} __attribute__((packed));
```

To soft-block all Bluetooth radios:

```c
int fd = open("/dev/rfkill", O_RDWR);
struct rfkill_event_ext ev = {
    .type = RFKILL_TYPE_BLUETOOTH,
    .op   = RFKILL_OP_CHANGE_ALL,
    .soft = 1,                       /* block */
};
write(fd, &ev, sizeof(ev));
```

To listen for events (radio hot-keys, hardware switch flips): `poll()`
on `/dev/rfkill` and read `rfkill_event` records.

The extended event structure requires opting in via:

```c
ioctl(fd, RFKILL_IOCTL_MAX_SIZE, sizeof(struct rfkill_event_ext));
```

Otherwise older kernels and buggy userspace (`systemd`, `bluez`,
`gnome-settings-daemon`) silently truncate. See the "Extensibility"
comment in `include/uapi/linux/rfkill.h`.

`RFKILL_IOCTL_NOINPUT` (`_IO('R', 1)`) disables the in-kernel rfkill
input handler that turns hotkey events into `CHANGE_ALL`s — useful
when userspace wants to handle the hotkey itself.

### 3.10 Wireless regulatory force / domain hints

Drivers can hint a regulatory domain via `regulatory_hint()` (kernel
API). From userspace the only entry point is `NL80211_CMD_REQ_SET_REG`.
For self-managed wiphys, use `NL80211_CMD_SET_REG` with a full rule
list — there is no way to drive this from sysfs.

---

## 4. Firmware / BIOS Domain

### 4.1 EFI variables

Kernel source: `fs/efivarfs/`, `drivers/firmware/efi/vars.c`,
`include/linux/efi.h`, `Documentation/filesystems/efivarfs.rst`,
`Documentation/ABI/removed/sysfs-firmware-efi-vars`.

The old `/sys/firmware/efi/vars/` sysfs interface was **removed in March 2023**
(after being deprecated since September 2020). The replacement is
**efivarfs**, a dedicated filesystem normally mounted at:

```
/sys/firmware/efi/efivars
```

Mount manually if not already mounted:

```
mount -t efivarfs none /sys/firmware/efi/efivars
```

#### 4.1.1 File layout

Each EFI variable is a regular file named:

```
<NAME>-<GUID>
```

- `NAME` is the variable name as a UTF-8 string. Standard UEFI names are ASCII (e.g. `Boot0001`, `BootOrder`, `Setup`, `SecureBoot`, `PK`, `KEK`, `db`, `dbx`, `MokListRT`, `MokSBStateRT`, `Timeout`, `BootCurrent`, `OsIndicationsSupported`, `OsIndications`).
- `GUID` is the vendor GUID in mixed-endian `8-4-4-4-12` form, e.g. `8be4df61-93ca-11d2-aa0d-00e098032b8c` (the global EFI GUID).

Example full path:

```
/sys/firmware/efi/efivars/BootOrder-8be4df61-93ca-11d2-aa0d-00e098032b8c
```

#### 4.1.2 File contents — attribute prefix

Every file's first 4 bytes are the variable's attributes as a
**little-endian `u32`**, followed by the variable data:

```
+----------------------------------------+
| 4-byte attributes (LE u32)  | data ... |
+----------------------------------------+
```

When you `cat` the file, `hexdump -C` will show those first 4 bytes —
ignore them when parsing the payload. When you write to the file, you
must include them.

Attribute bits (from `include/linux/efi.h`):

| Bit | Mask | Name | Meaning |
|---|---|---|---|
| 0 | `0x00000001` | `EFI_VARIABLE_NON_VOLATILE` | Variable persists across reboot. Required for `BootOrder`, `Boot####`, etc. |
| 1 | `0x00000002` | `EFI_VARIABLE_BOOTSERVICE_ACCESS` | Accessible during boot services. |
| 2 | `0x00000004` | `EFI_VARIABLE_RUNTIME_ACCESS` | Accessible at runtime (from the OS). |
| 3 | `0x00000008` | `EFI_VARIABLE_HARDWARE_ERROR_RECORD` | Variable holds hardware error record. |
| 4 | `0x00000010` | `EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS` | Deprecated in UEFI 2.x. |
| 5 | `0x00000020` | `EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS` | Writes require a PKCS#7-signed payload with timestamp. |
| 6 | `0x00000040` | `EFI_VARIABLE_APPEND_WRITE` | Append to existing data instead of replacing. |

Typical variable attributes:

| Variable class | Attributes (hex) |
|---|---|
| Boot variables (`Boot####`, `BootOrder`, `BootCurrent`, `Timeout`) | `0x7` (NV + BS + RT) |
| Secure Boot keys (`PK`, `KEK`, `db`, `dbx`) | `0x7` (+ `0x20` for writes once Secure Boot is enabled) |
| MOK variables (`MokListRT`, `MokSBStateRT`, `MokIgnoreDB`) | `0x7` |
| `OsIndications` / `OsIndicationsSupported` | `0x7` |
| `SetupMode` (read-only) | `0x7` |

#### 4.1.3 Reading

```
# Just read the data; skip the first 4 bytes
dd if=/sys/firmware/efi/efivars/BootOrder-8be4df61-93ca-11d2-aa0d-00e098032b8c bs=1 skip=4 2>/dev/null | hexdump -C
```

Or in C:

```c
int fd = open("/sys/firmware/efi/efivars/BootOrder-8be4df61-93ca-11d2-aa0d-00e098032b8c", O_RDONLY);
__u32 attributes;
read(fd, &attributes, sizeof(attributes));
// then read the rest as the variable data
```

Reading requires read permission on the file. efivarfs files are
created with mode `0644` by default (root-owned), except for
non-standard variables which are created with the immutable flag
(`chattr +i` on the inode — see below).

#### 4.1.4 Writing

To create or replace a variable: open the file with `O_WRONLY` (or
`O_WRONLY|O_CREAT` for a new variable) and write `attributes || data`:

```c
__u32 attributes = 0x7;  // NV | BS | RT
write(fd, &attributes, sizeof(attributes));
write(fd, data, data_size);
```

Shell:

```
# Build the buffer (4-byte attr prefix + data) and write
printf '\x07\x00\x00\x00' > /tmp/var.bin
cat data.bin >> /tmp/var.bin
cp /tmp/var.bin /sys/firmware/efi/efivars/MyVar-12345678-1234-1234-1234-123456789abc
```

Writing requires root and `CAP_SYS_ADMIN` (efivarfs enforces this in
`fs/efivarfs/file.c::efivarfs_file_write`). The kernel validates that
the attributes are in `EFI_VARIABLE_MASK` (the seven bits above);
unknown bits cause `EINVAL`.

#### 4.1.5 Deleting

A variable is deleted by writing with `attributes=0` and zero data
length. efivarfs enforces this in two ways:

1. **Open with O_TRUNC** (or write 0 bytes): the file is removed from
   the filesystem.
2. **Write just 4 bytes of zero attributes** followed by EOF: also
   deletes.

Shell:

```
# Delete by writing 0 to attributes and 0 bytes of data
printf '\x00\x00\x00\x00' > /sys/firmware/efi/efivars/MyVar-12345678-1234-1234-1234-123456789abc

# Or: truncate the file
> /sys/firmware/efi/efivars/MyVar-12345678-1234-1234-1234-123456789abc
```

In `fs/efivarfs/file.c::efivarfs_file_release`, when `open_count`
hits 0 and `i_size == 0`, the file is unlinked and the variable is
deleted via `efivar_entry_set_get_size()` with `attributes=0`.

**Quirks:**

- Many firmware bugs cause systems to fail POST if a non-standard
  variable is deleted. To prevent accidental deletion, efivarfs
  creates non-standard (not in the UEFI spec list) variables with the
  **immutable inode flag** (`chattr +i` / `FS_IMMUTABLE_FL`). To
  delete: `chattr -i <file>` first, then `rm` it.
- For Secure Boot-protected variables (`PK`, `KEK`, `db`, `dbx`),
  writes require the `EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS`
  attribute (bit 5) and a properly-signed PKCS#7 payload. Otherwise
  the firmware rejects the write with `EFI_SECURITY_VIOLATION`.
- Some firmware refuses any write at all if `SetupMode == 1` (i.e.
  user mode) and no `PK` is enrolled. Read `SetupMode` (1 = setup
  mode, 0 = user mode) before trying to enroll keys.
- `OsIndications` bits: `0x1` = boot to firmware UI on next boot
  (EFI_OS_INDICATIONS_BOOT_TO_FW_UI). Write `0x1` to `OsIndications`
  to request a reboot into BIOS setup.
- The variable name in the filename is the **UTF-8** name; UEFI
  stores it as UCS-2 LE internally. Non-ASCII names appear as `?` or
  as their UTF-8 byte sequence depending on the locale.

#### 4.1.6 File permissions on efivarfs

| Variable class | Mode | Flags |
|---|---|---|
| Standard UEFI vars (Boot*, BootOrder, BootCurrent, Timeout, SetupMode, SecureBoot, etc.) | `0644` | none |
| Non-standard / vendor vars | `0644` | `FS_IMMUTABLE_FL` (immutable) — `chattr -i` to allow modification |
| All vars | owner = root, group = root | |

### 4.2 Other EFI sysfs

Beyond efivars, `/sys/firmware/efi/` exposes (from
`Documentation/ABI/testing/sysfs-firmware-efi` and the related files):

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/firmware/efi/fw_vendor` | RO | hex | Physical address of the EFI firmware vendor string. |
| `/sys/firmware/efi/runtime` | RO | hex | Physical address of the EFI runtime services table. |
| `/sys/firmware/efi/config_table` | RO | hex | Physical address of the EFI config table. |
| `/sys/firmware/efi/systab` | RO | formatted | All EFI config tables, newer-first, e.g. `ACPI20=0x...` `ACPI=0x...` `SMBIOS=0x...` `SMBIOS3=0x...`. |
| `/sys/firmware/efi/tables/rci2` | RO | binary | Dell RCI2 table content. |
| `/sys/firmware/efi/ovmf_debug_log` | RO | text | OVMF debug log buffer (only if firmware supports it). |
| `/sys/firmware/efi/efivars/` | mount point | efivarfs | See 4.1. |
| `/sys/firmware/efi/esrt/` | dir | | EFI System Resource Table. See 4.3. |
| `/sys/firmware/efi/runtime-map/` | dir | | EFI runtime memory map (one file per region). |

### 4.3 EFI System Resource Table (ESRT)

`/sys/firmware/efi/esrt/` exposes firmware update capsule metadata
(from `Documentation/ABI/testing/sysfs-firmware-efi-esrt`):

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/firmware/efi/esrt/fw_resource_count` | RO | decimal | Number of entries. |
| `/sys/firmware/efi/esrt/fw_resource_count_max` | RO | decimal | Max entries the allocation could hold. |
| `/sys/firmware/efi/esrt/fw_resource_version` | RO | hex | ESRT structure version. |
| `/sys/firmware/efi/esrt/entries/entry<N>/fw_class` | RO | GUID | The entry's GUID (matches dir name). |
| `/sys/firmware/efi/esrt/entries/entry<N>/fw_type` | RO | decimal | 0 = unknown, 1 = System Firmware, 2 = Device Firmware, 3 = UEFI Driver. |
| `/sys/firmware/efi/esrt/entries/entry<N>/fw_version` | RO | decimal | Currently-installed version. |
| `/sys/firmware/efi/esrt/entries/entry<N>/lowest_supported_fw_version` | RO | decimal | Lowest acceptable version. |
| `/sys/firmware/efi/esrt/entries/entry<N>/capsule_flags` | RO | hex | Flags for `UpdateCapsule()`. |
| `/sys/firmware/efi/esrt/entries/entry<N>/last_attempt_version` | RO | decimal | Last attempted update version. |
| `/sys/firmware/efi/esrt/entries/entry<N>/last_attempt_status` | RO | decimal | 0 = success, 1 = insufficient resources, 2 = incorrect version, 3 = invalid format, 4 = auth error, 5 = power event, 6 = insufficient battery. |

### 4.4 DMI / SMBIOS

Kernel source: `drivers/firmware/dmi-id.c`, `drivers/firmware/dmi-sysfs.c`,
`drivers/firmware/dmi_scan.c`. ABI:
`Documentation/ABI/testing/sysfs-firmware-dmi-tables`,
`Documentation/ABI/testing/sysfs-firmware-dmi-entries`.

#### 4.4.1 /sys/class/dmi/id/

Friendly per-record attributes:

| Path | R/W | Mode | Notes |
|---|---|---|---|
| `bios_vendor` | RO | `0444` | |
| `bios_version` | RO | `0444` | |
| `bios_date` | RO | `0444` | MM/DD/YYYY. |
| `bios_release` | RO | `0444` | BIOS revision (extended). |
| `ec_firmware_release` | RO | `0444` | EC firmware revision. |
| `sys_vendor` | RO | `0444` | |
| `product_name` | RO | `0444` | |
| `product_version` | RO | `0444` | |
| `product_serial` | RO | `0400` | Root-only. PII. |
| `product_uuid` | RO | `0400` | Root-only. PII. |
| `product_sku` | RO | `0444` | |
| `product_family` | RO | `0444` | |
| `board_vendor` | RO | `0444` | |
| `board_name` | RO | `0444` | |
| `board_version` | RO | `0444` | |
| `board_serial` | RO | `0400` | Root-only. PII. |
| `board_asset_tag` | RO | `0444` | |
| `chassis_vendor` | RO | `0444` | |
| `chassis_type` | RO | `0444` | Decimal SMBIOS chassis type (1=Other, 2=Unknown, 3=Desktop, 4=LP, 5=Tower, 6=Server, 7=Server-class, 8=Ent. PC, 9=Mini, 10=All-in-one, 11=Notebook, 12=Handheld, 13=Docking, 14=All-in-one-2, 15=Sub, 16=Space-saving, 17=Lunch box, 18=Main server, 19=Blade server, 20=Blade enclosure, 21=Tablet, 22=Convertible, 23=Detachable, 24=IoT gateway, 25=Embedded, 26=Mini PC, 27=Stick PC). |
| `chassis_version` | RO | `0444` | |
| `chassis_serial` | RO | `0400` | Root-only. PII. |
| `chassis_asset_tag` | RO | `0444` | |
| `uevent` | RO | | Standard. |

`chassis_type` is the SMBIOS `Type 3` field; not all chassis fill in
the serial / asset tag fields correctly. The `*_serial` and
`product_uuid` files are `0400` because they expose hardware
identifiers and the kernel considers them PII.

Files that are not present on the platform return ENOENT (the kernel
omits them rather than emitting empty strings).

#### 4.4.2 /sys/firmware/dmi/tables/

Raw SMBIOS access (no parsing by the kernel):

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/firmware/dmi/tables/smbios_entry_point` | RO | binary | The SMBIOS entry point (32-bit `_SM_` or 64-bit `_SM3_`). |
| `/sys/firmware/dmi/tables/DMI` | RO | binary | The full DMI table (concatenation of all SMBIOS structures). |

`dmidecode --type memory` reads these files directly when /dev/mem
access is unavailable.

#### 4.4.3 /sys/firmware/dmi/entries/

Per-record directories, one per SMBIOS structure:

```
/sys/firmware/dmi/entries/<type>-<instance>/
```

e.g. `17-0` for the first memory device (Type 17). Each entry has:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `handle` | RO | hex | 16-bit firmware-assigned handle. |
| `length` | RO | decimal | Length of the formatted region. |
| `type` | RO | decimal | SMBIOS structure type (matches dir name). |
| `instance` | RO | decimal | Ordinal among entries of the same type. |
| `position` | RO | decimal | Position in the full table. |
| `raw` | RO | binary | Full entry: formatted region + string set + two terminating NULs. |

For well-known types the kernel also creates a per-type subdir with
named attributes. For Type 0 (BIOS Information): `bios_vendor`,
`bios_version`, `bios_release_date`, `bios_revision`,
`firmware_revision`, `address`, `runtime_size`, `rom_size`. For Type
1 (System Information): `manufacturer`, `product_name`,
`product_version`, `serial_number`, `uuid`, `wake_up_type`,
`sku_number`, `family`. For Type 2 (Baseboard): `manufacturer`,
`product_name`, `version`, `serial_number`, `asset_tag`. For Type 3
(Chassis): `manufacturer`, `type`, `version`, `serial_number`,
`asset_tag`, `bootup_state`, `power_supply_state`,
`thermal_state`, `security_status`, `oem_information`. For Type 17
(Memory Device): `locator`, `bank_locator`, `memory_type`,
`speed`, `size`, plus a few vendor-specifics. The full list is in
`drivers/firmware/dmi-sysfs.c`.

### 4.5 ACPI tables

Kernel source: `drivers/acpi/tables.c`, `drivers/acpi/sysfs.c`. ABI:
`Documentation/ABI/testing/sysfs-firmware-acpi`.

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/firmware/acpi/tables/` | dir | | One binary file per ACPI table (DSDT, SSDT, FACP, APIC, MCFG, HPET, FPDT, BGRT, etc.). The file is the raw ACPICA table. |
| `/sys/firmware/acpi/tables/DSDT` | RO | binary | The Differentiated System Description Table (AML bytecode). Disassemble with `iasl -d`. |
| `/sys/firmware/acpi/tables/SSDT` | RO | binary | The static system description table. May be multiple files (`SSDT1`, `SSDT2`, ...). |
| `/sys/firmware/acpi/tables/FACP` | RO | binary | The Fixed ACPI Description Table (FADT). |
| `/sys/firmware/acpi/tables/dynamic/` | dir | | Dynamically loaded SSDTs (loaded via `ConfigFS` or `initrd_table_override`). |
| `/sys/firmware/acpi/tables/data/` | dir | | Optional table data. |
| `/sys/firmware/acpi/bgrt/` | dir | | Boot Graphics Resource Table. `image` (BMP binary), `status`, `type`, `version`, `xoffset`, `yoffset`. |
| `/sys/firmware/acpi/fpdt/` | dir | | Firmware Performance Data Table. `boot/`, `suspend/`, `resume/` subdirs with `*_ns` timestamp files plus raw `FBPT` and `S3PT` binaries. |
| `/sys/firmware/acpi/hotplug/<class>/enabled` | RW | `0` / `1` | Per-class ACPI hotplug enable (containers, memory, processors, PCI root). |
| `/sys/firmware/acpi/interrupts/` | dir | | Per-GPE counters + `sci`, `sci_not`, `gpe_all`, `error`. Each file is `count<tab>state` where state is `enable` / `disable` / `invalid`. Root can clear (`echo 0 > gpe11`) or toggle (`echo enable\|disable\|clear > gpe11`). |
| `/sys/firmware/acpi/memory_ranges/range<N>/` | dir | | ACPI MRRM table memory range info (kernel 6.12+). `base`, `length`, `node`, `local_region_id`, `remote_region_id`. |
| `/sys/firmware/acpi/ospm/...` | dir | | OSPM features. |
| `/sys/firmware/acpi/pm_profile` | RO | decimal | `acpi_gbl_FADT.preferred_profile` (0=Unspecified, 1=Desktop, 2=Mobile, 3=Workstation, 4=Enterprise Server, 5=SOHO Server, 6=Appliance PC, 7=Performance Server, 8=Tablet, 9=Undefined). |

ACPI tables can be upgraded via initrd:
`Documentation/admin-guide/acpi/initrd_table_override.rst`. ACPI SSDTs
can be loaded at runtime via ConfigFS:
`Documentation/admin-guide/acpi/ssdt-overlays.rst`.

### 4.6 ACPI events

Kernel source: `drivers/acpi/event.c`, `drivers/acpi/button.c`.

The legacy `/proc/acpi/event` was **removed** in modern kernels; it
was a character device that broadcast ACPI events as text. The modern
path is:

#### 4.6.1 netlink ACPI events

`drivers/acpi/event.c` registers a generic netlink family `acpi_event`
with multicast group `acpi_mc_group`. To receive events:

```c
int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
struct sockaddr_nl sa = { .nl_family = AF_NETLINK,
                          .nl_groups = 1 << 0 /* acpi_mc_group */ };
bind(fd, (struct sockaddr *)&sa, sizeof(sa));
struct {
    struct nlmsghdr nh;
    struct genlmsghdr gh;
    struct nlattr attr;
    struct acpi_genl_event ev;
} msg;
recv(fd, &msg, sizeof(msg), 0);
// msg.ev.device_class = "button/lid" / "ac_adapter" / "battery" / ...
// msg.ev.bus_id = "LNXFILL..." (ACPI device bus ID)
// msg.ev.type = event code (e.g. 0x80 for Notify)
// msg.ev.data = event data
```

`acpid` (the ACPI daemon) listens on this netlink group and dispatches
events to `/etc/acpi/events/*` rules. `acpi_listen` is the
user-space tool that prints events as they arrive.

#### 4.6.2 ACPI buttons

ACPI button drivers (`drivers/acpi/button.c`) emit both:

- Input events (`/dev/input/eventN`): `KEY_POWER`, `KEY_SLEEP`,
  `SW_LID` (lid switch). These are the primary interface for
  userspace.
- Netlink ACPI events (above) for legacy compat.

For lid state, the driver still creates a procfs entry as a fallback:

```
/proc/acpi/button/lid/<ACPI_DEVICE_ID>/state    # "open" / "closed"
```

For example `/proc/acpi/button/lid/LID0/state` returns `state:   open`
or `state:   closed`. The driver parameter `lid_init_state` controls
how the initial lid state is reported (`ignore`, `open`, `method`,
`disabled`); see `drivers/acpi/button.c`.

The power button has no per-button sysfs file; it just emits
`KEY_POWER` on its input device.

#### 4.6.3 ACPI wakeup sources

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/proc/acpi/wakeup` | RO | formatted table | Lists wakeup-capable devices (`Device`, `S-state`, `Status`, `Sysfs node`). Writing the device name (e.g. `echo PBTN > /proc/acpi/wakeup`) toggles its `enabled` flag. |
| `/sys/devices/<acpi_dev>/power/wakeup` | RW | `enabled` / `disabled` | Same effect via sysfs. |

### 4.7 WMI (Windows Management Instrumentation)

WMI is a Microsoft extension of ACPI used by most laptop vendors to
expose hotkey events and BIOS configuration knobs. Kernel source:
`drivers/platform/wmi/`, `drivers/platform/x86/wmi.c` (legacy),
`include/linux/wmi.h`. Docs: `Documentation/wmi/`,
`Documentation/driver-api/wmi.rst`.

Modern kernels expose WMI devices on a bus:

```
/sys/bus/wmi/devices/<GUID>[-<instance>]/
```

e.g. `/sys/bus/wmi/devices/05901221-D566-11D1-B2F0-00A0C9062910/`.

GUIDs are 128-bit Microsoft Variant 2 GUIDs, displayed in
`8-4-4-4-12` form. The same GUID can appear multiple times on a
system; the optional `-<instance>` suffix disambiguates.

**The old `/sys/class/wmi/` interface is removed.** Use
`/sys/bus/wmi/devices/`.

Per-device attributes (from `drivers/platform/wmi/core.c`):

| Path | R/W | Format | Notes |
|---|---|---|---|
| `guid` | RO | GUID string | e.g. `05901221-D566-11D1-B2F0-00A0C9062910`. |
| `modalias` | RO | `wmi:XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` | For module autoloading. |
| `instance_count` | RO | decimal | Number of WMI object instances. |
| `expensive` | RO | `0` / `1` | `1` if querying this device is expensive (the kernel auto-enables/disables the `_WED` ACPI method). |
| `object_id` | RO | 2 ASCII chars | Used internally to construct ACPI method names (`WQxx`, `WSxx`). Only present for data-block WMI devices. |
| `notify_id` | RO | hex | 2-byte notify ID. Only present for event WMI devices. |
| `setable` | RO | `0` / `1` | `1` if the data block supports `WSxx` (Set Control Method). Only present for data-block WMI devices. |
| `driver_override` | RW | string | Force-bind a specific driver (write `wmi-event-dummy` etc., or empty to clear). |
| `bmof` | RO | binary | Binary MOF metadata. Only on the `05901221-...` bmof GUID; loaded by the `wmi-bmof` driver. Use `bmf2mof` to decode. |

WMI drivers bind to specific GUIDs and expose vendor-specific
attributes under `/sys/devices/platform/<platform>/` (legacy) or
`/sys/bus/wmi/devices/<guid>/` (modern). The `firmware-attributes`
class (next section) is the cross-vendor unification.

### 4.8 firmware-attributes — unified BIOS settings

Kernel source: `drivers/platform/x86/firmware_attributes_class.c`.
ABI: `Documentation/ABI/testing/sysfs-class-firmware-attributes`.

This is the cross-vendor class for BIOS configuration. Vendors
implementing it: Dell (`dell-wmi-sysman`), Lenovo (`think-lmi`),
HP (`hp-bioscfg`), Alienware, and others. The class appears at:

```
/sys/class/firmware-attributes/<vendor>/
```

with subdirs:

```
attributes/
authentication/
attributes/reset_bios
attributes/pending_reboot
attributes/save_settings         (Lenovo, kernel 6.6+)
```

#### 4.8.1 attributes/

Each setting is a directory:

```
/sys/class/firmware-attributes/<vendor>/attributes/<setting>/
```

with:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `type` | RO | `enumeration` / `integer` / `string` / `ordered-list` (HP) | Attribute type. Mandatory. |
| `current_value` | RW | string | Read current; write new value. Mandatory. |
| `default_value` | RO | string | Factory default. |
| `display_name` | RO | string | User-facing label. |
| `display_name_language_code` | RO | string | IETF language tag (e.g. `en`). |
| `possible_values` | RO | `;`-separated list | For `enumeration`. |
| `min_value` / `max_value` / `scalar_increment` | RO | decimal | For `integer`. |
| `max_length` / `min_length` | RO | decimal | For `string`. |
| `elements` | RO | `;`-separated list | For `ordered-list` (HP). Write `current_value` with reordered list to change priority. |
| `dell_modifier` | RO | string | Dell-only: per-attribute dependency rule (`[ReadOnlyIf:X=Y]`, `[SuppressIf:X=Y]`, etc.). |
| `dell_value_modifier` | RO | string | Dell-only: per-value dependency. |
| `lenovo_encoding` | RW | `ascii` / `scancode` | Lenovo-only. |
| `lenovo_kbdlang` | RW | 2-char code | Lenovo-only. |

#### 4.8.2 authentication/

For platforms that require a BIOS admin password:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `is_enabled` | RO | `0` / `1` | Whether authentication is set. Mandatory. |
| `role` | RO | `bios-admin` / `power-on` / `system-mgmt` / `HDD` / `NVMe` / `enhanced-bios-auth` (HP) | Authentication role. |
| `mechanism` | RO | `password` / `certificate` | Auth mechanism. Mandatory. |
| `max_password_length` | RO | decimal | |
| `min_password_length` | RO | decimal | |
| `current_password` | WO | string | Required for privileged operations when a password is set. |
| `new_password` | WO | string | Set or reset password (paired with `current_password`). Empty value clears. |
| `certificate` | RW | BASE64 | For `certificate` mechanism (Lenovo 2025+). |
| `signature` | WO | BASE64 | For certificate-based auth. |
| `save_signature` | WO | BASE64 | Lenovo. |
| `certificate_thumbprint` | RO | MD5/SHA1/SHA256 thumbprints | Lenovo. |
| `certificate_to_password` | WO | string | Switch from certificate back to password auth. |
| `level` | RW | `user` / `master` | Lenovo HDD/NVMe password privilege. |
| `index` | RW | decimal | Lenovo HDD/NVMe drive index. |

Session example (Dell):

```
echo "password" > /sys/class/firmware-attributes/dell-wmi-sysman/authentication/Admin/current_password
echo "disabled" > /sys/class/firmware-attributes/dell-wmi-sysman/attributes/TouchScreen/current_value
echo "" > /sys/class/firmware-attributes/dell-wmi-sysman/authentication/Admin/current_password
```

#### 4.8.3 reset_bios

```
/sys/class/firmware-attributes/<vendor>/attributes/reset_bios
```

Reading returns the supported reset modes with the active one in
brackets:

```
builtinsafe lastknowngood [factory] custom
```

Write one of the modes to request it. The reset takes effect on the
next reboot.

#### 4.8.4 pending_reboot

```
/sys/class/firmware-attributes/<vendor>/attributes/pending_reboot
```

Reads `1` if a reboot is needed to apply pending changes, `0`
otherwise. The driver emits a `KOBJ_CHANGE` uevent when this flips to
`1`.

#### 4.8.5 save_settings (Lenovo, kernel 6.6+)

Lenovo has an architectural limit of 48 attribute saves. The
`save_settings` attribute lets users batch saves: write `single` to
save after every change (legacy behaviour), write `bulk` to defer
saves, then write `save` to commit all pending changes.

### 4.9 Vendor-specific kernel modules

Most laptops still expose vendor-specific BIOS / EC controls through
dedicated drivers in `drivers/platform/x86/`. Notable ones:

| Driver | Module | Sysfs path | Notable features |
|---|---|---|---|
| `thinkpad_acpi` | `thinkpad_acpi` | `/sys/devices/platform/thinkpad_acpi/`, `/proc/acpi/ibm/` | Hotkeys, fan control (`fan1_input`, `pwm1`, `pwm1_enable`), thermal sensors, Bluetooth (`/proc/acpi/ibm/bluetooth`, `bluetooth_enable` attr), WWAN, UWB, LED control, volume/mute, migration from `/proc/acpi/ibm/` to sysfs is partial. |
| `think-lmi` | `think-lmi` | `/sys/class/firmware-attributes/thinklmi/` | Lenovo Think BIOS settings via WMI (firmware-attributes class). |
| `dell-wmi-sysman` | `dell_wmi_sysman` | `/sys/class/firmware-attributes/dell-wmi-sysman/` | Dell BIOS settings via WMI. |
| `dell-wmi-ddv` | `dell_wmi_ddv` | `/sys/bus/wmi/devices/.../` | Dell battery diagnostics, ePPID, thermal sensors. |
| `dell-smbios-wmi` | `dell_smbios_wmi` | `/sys/bus/wmi/devices/.../` | Dell SMBIOS settings via WMI. |
| `dell-laptop` | `dell_laptop` | rfkill `dell-wifi` etc. | Radio control, brightness. |
| `hp-wmi` | `hp_wmi` | `/sys/devices/platform/hp-wmi/`, rfkill `hp-wifi` / `hp-bluetooth` / `hp-wwan` / `hp-softmac` | HP radio switches, brightness, dock, ALS, 3G. |
| `hp-bioscfg` | `hp_bioscfg` | `/sys/class/firmware-attributes/hp-bioscfg/` | HP BIOS settings via WMI (firmware-attributes class). |
| `asus-wmi` | `asus_wmi` | `/sys/devices/platform/asus-nb-wmi/`, `/sys/devices/platform/asus_laptop/` | ASUS BIOS settings, fan mode, CPU overclock, touchpad, camera, card reader, etc. Per-attr: `cpufv`, `camera`, `cardr`, `touchpad`, `lid_resume`, `fan_boost_mode`, etc. |
| `asus-armoury` | `asus_armoury` | `/sys/devices/platform/asus-armoury/` | ASUS Armoury Crate attributes (TUF/ROG, kernel 6.10+). |
| `huawei-wmi` | `huawei_wmi` | `/sys/devices/platform/huawei-wmi/` | Huawei micmute LED, battery charge thresholds, Fn-lock. |
| `ideapad-laptop` | `ideapad_laptop` | `/sys/bus/platform/devices/.../` | Lenovo IdeaPad: `bluetooth_power`, `wifi_power`, `camera_power`, `touchpad_power`, `conservation_mode` (battery charge limit). |
| `msi-wmi` | `msi_wmi` | `/sys/devices/platform/msi-wmi/` | MSI hotkeys. |
| `msi-wmi-platform` | `msi_wmi_platform` | `/sys/bus/wmi/devices/.../` | MSI WMI platform (newer). |
| `acer-wmi` | `acer_wmi` | `/sys/devices/platform/acer-wmi/` | Acer radio, mail LED, brightness, bluetooth. |
| `alienware-wmi` | `alienware_wmi` | `/sys/devices/platform/alienware-wmi/` | Alienware LED zones. |
| `intel-wmi-thunderbolt` | `intel_wmi_thunderbolt` | `/sys/bus/wmi/devices/86CCFD48-205E-4A77-9C48-2021CBEDE341[-X]/force_power` | WO (write-only) `0` / `1` to force Thunderbolt controller power on/off. |
| `intel-wmi-sbl-fw-update` | `intel_wmi_sbl_fw_update` | `/sys/bus/wmi/devices/.../fwupdate_done`, `fwupdate_res`, `fwupdate_trigger` | Intel Slim Bootloader firmware update. |
| `wmi-bmof` | `wmi_bmof` | `/sys/bus/wmi/devices/05901221-D566-11D1-B2F0-00A0C9062910[-X]/bmof` | Binary MOF metadata. |
| `xiaomi-wmi` | `xiaomi_wmi` | | Xiaomi hotkeys. |
| `gigabyte-wmi` | `gigabyte_wmi` | | Gigabyte motherboard sensors. |
| `toshiba-wmi` | `toshiba_wmi` | | Toshiba hotkeys. |
| `topstar-laptop` | `topstar_laptop` | | Topstar hotkeys. |

User-space vendor tools that talk directly to firmware without kernel
drivers (mentioned for completeness):

- **Lenovo `thinkpad-ec`**: not a kernel module; `tpctl` and
  `nvramtool` access the ThinkPad embedded controller and NVRAM
  directly via I/O ports. `thinkpad_acpi` covers most of what `tpctl`
  did.
- **Dell `libsmbios`**: userspace library that talks to Dell SMBIOS
  via WMI or SMI. The kernel `dell-smbios-wmi` driver supersedes it
  for BIOS attribute access.
- **HP `hp-wmi`** userspace (CoolSense, HP System Event Utility): the
  kernel `hp-wmi` driver covers the essentials.

### 4.10 Firmware loading (firmware_class)

Kernel source: `drivers/base/firmware_loader/`,
`Documentation/driver-api/firmware/`. ABI:
`Documentation/ABI/testing/sysfs-class-firmware`.

The firmware loader exposes a sysfs class for two purposes:

1. **Fallback loading**: when direct filesystem lookup fails, the
   kernel asks userspace to provide the blob via a uevent.
2. **Firmware upload**: persistent per-device nodes for drivers that
   support live firmware updates (FPGAs, BMCs, etc.).

#### 4.10.1 Fallback: /sys/class/firmware/

When a driver calls `request_firmware()` and direct lookup fails, the
firmware loader creates a temporary device:

```
/sys/class/firmware/<name>/
    data          # binary, write-only
    loading       # RW: 0 = done, 1 = loading, -1 = abort
    timeout       # RW: seconds (default 60)
```

Plus, when `CONFIG_FW_UPLOAD=y`:

```
    status            # RO: idle / receiving / preparing / transferring / programming
    error             # RO: <STATUS>:<ERROR> when status is idle
    cancel            # WO: 1 cancels the upload
    remaining_size    # RO: bytes remaining
```

Loading sequence (from
`Documentation/driver-api/firmware/fallback-mechanisms.rst`):

```
echo 1 > /sys/class/firmware/<name>/loading
cp /lib/firmware/<blob> /sys/class/firmware/<name>/data
echo 0 > /sys/class/firmware/<name>/loading
```

Modern distributions use only direct filesystem lookup (the path list
below); the fallback mechanism is rarely triggered because
`CONFIG_FW_LOADER_USER_HELPER_FALLBACK` is usually off.

#### 4.10.2 Search paths

From `Documentation/driver-api/firmware/fw_search_path.rst`, in order:

1. `/sys/module/firmware_class/parameters/path` (module param `path`,
   custom override).
2. `/lib/firmware/updates/<UTS_RELEASE>/` (where `<UTS_RELEASE>` is
   the kernel's `uname -r`).
3. `/lib/firmware/updates/`
4. `/lib/firmware/<UTS_RELEASE>/`
5. `/lib/firmware/`

The custom path module parameter:

```
/sys/module/firmware_class/parameters/path
```

Write the path with `echo -n /path/to/dir > ...` (newline will be
included in the path and break lookups).

#### 4.10.3 Module parameters and sysctls

| Path | Format | Notes |
|---|---|---|
| `/sys/module/firmware_class/parameters/path` | string | Custom search path. See above. |
| `/proc/sys/kernel/firmware_config/force_sysfs_fallback` | `0` / `1` | Force the sysfs fallback even when direct lookup would succeed. |
| `/proc/sys/kernel/firmware_config/ignore_sysfs_fallback` | `0` / `1` | Ignore fallback requests entirely. |
| `/sys/class/firmware_class/timeout` (class attr) | int seconds | Default firmware loading timeout. `0` = wait forever. Default 60. |

### 4.11 Misc firmware sysfs

#### 4.11.1 /sys/devices/.../firmware_node/

For any ACPI-backed device, the `firmware_node` symlink points at the
ACPI device node, which exposes:

| Path | R/W | Format | Notes |
|---|---|---|---|
| `description` | RO | string | ACPI `_STR` method return. Only present if the device has `_STR`. |
| `hid` | RO | string | ACPI hardware ID (e.g. `PNP0C14` for WMI). |
| `uid` | RO | string | ACPI unique ID. |
| `path` | RO | string | ACPI namespace path (e.g. `\_SB.PCI0.LPCB.EC0`). |
| `modalias` | RO | `acpi:<HID>:` or `of:<of-node-name>` | For module autoloading. |
| `status` | RO | decimal | `acpi_device_status`. 1 = present, 0 = absent. |
| `real_power_state` | RO | decimal | Real ACPI power state. |
| `power/resources*/` | dir | | Power resource state. |
| `suppliers/`, `consumers/` | symlinks | | ACPI `_DEP` relationships. |

#### 4.11.2 /sys/firmware/memmap/

Raw BIOS-provided memory map (E820):

| Path | R/W | Format | Notes |
|---|---|---|---|
| `/sys/firmware/memmap/<N>/start` | RO | hex | Physical address. |
| `/sys/firmware/memmap/<N>/end` | RO | hex | Last byte address (inclusive). |
| `/sys/firmware/memmap/<N>/type` | RO | string | `RAM` / `Reserved` / `ACPI Tables` / `ACPI Non-volatile Storage` / `Unusable memory` / etc. |

#### 4.11.3 /sys/firmware/acpi/tables/dynamic/

Dynamically-loaded SSDTs land here after a successful `ConfigFS` or
`initrd_table_override` load. Each file is the raw AML table.

### 4.12 EFI stub

For kernels built with `CONFIG_EFI_STUB`, the bzImage itself is a
valid PE/COFF executable that EFI firmware can boot directly (no need
for GRUB). See `Documentation/admin-guide/efi-stub.rst`.

Boot params of note (passed after the bzImage on the EFI shell
command line):

- `initrd=\path\to\initrd` — absolute path on the ESP, backslash
  separators.
- `dtb=\path\to\fdt` (arm64) — device tree blob.
- All standard kernel cmdline args.

This is boot-time only; not a sysfs / runtime interface.

---

## 5. Capability detection cheat sheet

For Zenctl's init-time capability probe, check these existence /
permission flags:

| Domain | Capability | Detection |
|---|---|---|
| USB | sysfs attrs | `/sys/bus/usb/devices/` exists. |
| USB | per-device `power/control` | `access(..., W_OK) == 0` on `power/control`. |
| USB | `power/autosuspend_delay_ms` | file exists. |
| USB | `power/persist` | file exists (absent for hubs). |
| USB | `power/wakeup` | file exists; non-empty = capable. |
| USB | `power/usb2_hardware_lpm` | file exists if device supports LPM. |
| USB | per-device `authorized` | file exists. |
| USB | `USBDEVFS_RESET` | `/dev/bus/usb/<bus>/<dev>` openable `O_RDWR`. |
| USB | port power off | `<port>/power/pm_qos_no_power_off` exists. |
| BT | adapter present | `/sys/class/bluetooth/hci<N>/` exists. |
| BT | rfkill | rfkill device with `type=bluetooth` exists. |
| BT | mgmt socket | `socket(AF_BLUETOOTH, ..., BTPROTO_HCI)` succeeds with `HCI_CHANNEL_CONTROL`. |
| BT | HCI socket | `socket(AF_BLUETOOTH, ..., BTPROTO_HCI)` succeeds with `HCI_CHANNEL_RAW`. |
| BT | BlueZ D-Bus | D-Bus name `org.bluez` is registered. |
| BT | USB adapter PM | Adapter's parent USB interface has `power/control` (section 2.3). |
| Wireless | wiphy present | `/sys/class/ieee80211/phy<N>/` exists. |
| Wireless | nl80211 | `genl_ctrl_resolve(sock, "nl80211") >= 0`. |
| Wireless | TX power | `iw phy <phy> info` lists `set_wiphy_netns` / `set_tx_power` in `supported interface commands`. |
| Wireless | power_save | `iw phy <phy> info` lists `set_power_save`. |
| Wireless | regdomain | `/lib/firmware/regulatory.db` exists (or self-managed). |
| Wireless | WEXT | `SIOCGIWNAME` ioctl succeeds on the interface. |
| rfkill | rfkill present | `/sys/class/rfkill/` exists; `/dev/rfkill` openable. |
| EFI | efivarfs mounted | `statfs("/sys/firmware/efi/efivars", ...)` `f_type == EFIVARFS_MAGIC` (`0xde5e81e4`). |
| EFI | EFI runtime | `/sys/firmware/efi/` exists. |
| EFI | Secure Boot | `od -An -tu1 /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c | tail -c 1` returns `1` for Secure Boot on. (`SetupMode` byte similarly: 1 = setup mode.) |
| DMI | DMI present | `/sys/class/dmi/id/` exists. |
| ACPI | ACPI tables | `/sys/firmware/acpi/tables/` exists. |
| ACPI | ACPI events | `socket(AF_NETLINK, ..., NETLINK_GENERIC)` + bind to `acpi_mc_group`. |
| WMI | WMI devices | `/sys/bus/wmi/devices/` exists and is non-empty. |
| Firmware-attrs | BIOS settings class | `/sys/class/firmware-attributes/` exists. |
| Vendor | thinkpad | `/sys/devices/platform/thinkpad_acpi/` exists. |
| Vendor | dell-wmi | `/sys/bus/wmi/devices/` has Dell GUIDs; `/sys/class/firmware-attributes/dell-wmi-sysman/` exists. |
| Vendor | hp-wmi | `/sys/devices/platform/hp-wmi/` exists. |
| Vendor | asus-wmi | `/sys/devices/platform/asus-nb-wmi/` or `/sys/devices/platform/asus_laptop/` exists. |
| Vendor | huawei-wmi | `/sys/devices/platform/huawei-wmi/` exists. |
| Vendor | think-lmi | `/sys/class/firmware-attributes/thinklmi/` exists. |
| Vendor | hp-bioscfg | `/sys/class/firmware-attributes/hp-bioscfg/` exists. |
| Firmware loader | firmware_class | `/sys/module/firmware_class/` exists. |
| Firmware loader | fw upload | `CONFIG_FW_UPLOAD` => `/sys/class/firmware/.../status` exists for registered upload devices. |

---

## 6. Cross-references to kernel source

Authoritative source for every path above:

| Section | Files |
|---|---|
| USB device attrs | `Documentation/ABI/testing/sysfs-bus-usb`, `Documentation/ABI/stable/sysfs-bus-usb`, `Documentation/ABI/obsolete/sysfs-bus-usb`, `drivers/usb/core/sysfs.c`, `drivers/usb/core/port.c`, `drivers/usb/core/hub.c` |
| USB PM | `Documentation/driver-api/usb/power-management.rst`, `Documentation/driver-api/usb/persist.rst`, `drivers/usb/core/driver.c`, `drivers/base/power/sysfs.c` |
| USB authorization | `Documentation/usb/authorization.rst`, `drivers/usb/core/sysfs.c` |
| USB ioctls | `include/uapi/linux/usbdevice_fs.h`, `include/uapi/linux/usb/ch9.h`, `drivers/usb/core/devio.c` |
| Bluetooth sysfs | `net/bluetooth/hci_sysfs.c`, `Documentation/ABI/stable/sysfs-class-bluetooth` |
| Bluetooth debugfs | `net/bluetooth/hci_debugfs.c` |
| Bluetooth socket | `include/net/bluetooth/hci_sock.h`, `net/bluetooth/hci_sock.c` |
| Bluetooth mgmt | `include/net/bluetooth/mgmt.h`, `net/bluetooth/mgmt.c` |
| Bluetooth USB | `drivers/bluetooth/btusb.c` |
| rfkill | `include/uapi/linux/rfkill.h`, `include/linux/rfkill.h`, `net/rfkill/core.c`, `Documentation/ABI/stable/sysfs-class-rfkill`, `Documentation/driver-api/rfkill.rst` |
| Wireless sysfs | `net/wireless/sysfs.c`, `Documentation/ABI/stable/sysfs-class-ieee80211` (removed from tree, see git history) |
| nl80211 | `include/uapi/linux/nl80211.h`, `net/wireless/nl80211.c`, `net/wireless/core.c` |
| WEXT | `include/uapi/linux/wireless.h`, `net/core/wireless.c` |
| Regulatory | `Documentation/networking/regulatory.rst`, `net/wireless/reg.c`, `net/wireless/regdb.c` |
| EFI variables | `include/linux/efi.h`, `fs/efivarfs/`, `drivers/firmware/efi/vars.c`, `Documentation/filesystems/efivarfs.rst`, `Documentation/ABI/removed/sysfs-firmware-efi-vars` |
| EFI ESRT | `Documentation/ABI/testing/sysfs-firmware-efi-esrt`, `drivers/firmware/efi/esrt.c` |
| DMI | `drivers/firmware/dmi-id.c`, `drivers/firmware/dmi-sysfs.c`, `drivers/firmware/dmi_scan.c`, `Documentation/ABI/testing/sysfs-firmware-dmi-tables`, `Documentation/ABI/testing/sysfs-firmware-dmi-entries` |
| ACPI tables | `drivers/acpi/tables.c`, `drivers/acpi/sysfs.c`, `Documentation/ABI/testing/sysfs-firmware-acpi` |
| ACPI events | `drivers/acpi/event.c`, `drivers/acpi/button.c`, `drivers/acpi/proc.c` |
| ACPI button | `Documentation/firmware-guide/acpi/acpi-lid.rst`, `drivers/acpi/button.c` |
| WMI | `drivers/platform/wmi/`, `include/linux/wmi.h`, `Documentation/wmi/`, `Documentation/ABI/testing/sysfs-bus-wmi`, `Documentation/driver-api/wmi.rst` |
| firmware-attributes | `Documentation/ABI/testing/sysfs-class-firmware-attributes`, `drivers/platform/x86/firmware_attributes_class.c`, `drivers/platform/x86/lenovo/think-lmi.c`, `drivers/platform/x86/dell/dell-wmi-sysman/`, `drivers/platform/x86/hp/hp-bioscfg/` |
| Vendor modules | `drivers/platform/x86/lenovo/thinkpad_acpi.c`, `drivers/platform/x86/dell/dell-laptop.c`, `drivers/platform/x86/dell/dell-smbios-wmi.c`, `drivers/platform/x86/hp/hp-wmi.c`, `drivers/platform/x86/asus-wmi.c`, `drivers/platform/x86/huawei-wmi.c` |
| firmware_class | `drivers/base/firmware_loader/`, `Documentation/driver-api/firmware/`, `Documentation/ABI/testing/sysfs-class-firmware`, `Documentation/ABI/testing/sysfs-class-firmware-attributes` (for upload attrs) |
| EFI stub | `Documentation/admin-guide/efi-stub.rst`, `drivers/firmware/efi/libstub/` |

---

## 7. Open issues / things Zenctl must handle at runtime

1. **efivarfs not mounted.** Some minimal containers and embedded
   images mount `/sys/firmware/efi` but not `efivars`. Zenctl must
   `statfs()` and either auto-mount or report
   `ZENCTL_CAP_UNAVAILABLE`.

2. **efivarfs immutable flag.** Vendor variables created with
   `FS_IMMUTABLE_FL` cannot be modified or deleted without
   `chattr -i`. Zenctl must `ioctl(FS_IOC_GETFLAGS)` on the file
   before any write; if `FS_IMMUTABLE_FL` is set, clear it, write,
   then re-set it. Requires `CAP_LINUX_IMMUTABLE`.

3. **Secure Boot auth-writes.** Writes to `PK`, `KEK`, `db`, `dbx`
   with `EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS` need a
   PKCS#7-signed payload. Zenctl should not attempt to wrap these in
   v1; mark the capability `ZENCTL_CAP_READONLY` and tell the caller
   to use `mokutil` / `keytool` / `efi-signer`.

4. **Bluetooth over USB autosuspend.** `btusb` only calls
   `usb_enable_autosuspend()` at probe when
   `/sys/module/btusb/parameters/enable_autosuspend=1` (or
   `CONFIG_BT_HCIBTUSB_AUTOSUSPEND=y`). Zenctl writing
   `power/control=auto` does nothing unless the driver participates.
   Detect: read `/sys/module/btusb/parameters/enable_autosuspend`.

5. **Bluetooth D-Bus is required for pairing.** The kernel mgmt
   socket handles power / discoverability / settings; pairing requires
   an agent (`org.bluez.AgentManager1`) which lives in userspace.
   Zenctl should not implement pairing; only power / settings.

6. **Wireless "iw" deprecation warnings.** `iwconfig` and WEXT are
   deprecated; new kernels emit warnings in dmesg. Zenctl should use
   nl80211 exclusively, and fall back to WEXT only for ancient drivers
   (b43, old ipw2200) that don't have a cfg80211 backend.

7. **Self-managed regdomain drivers.** Some drivers (ath11k, mt7921)
   set `REGULATORY_WIPHY_SELF_MANAGED_REG`; `iw reg set <country>`
   does nothing for them. Detect via `iw phy <phy> info` flag
   `self-managed` and mark the regdomain capability as
   `ZENCTL_CAP_UNAVAILABLE` for those phys.

8. **WMI GUIDs are not unique.** The same GUID can appear multiple
   times; the `-<instance>` suffix matters. Zenctl must enumerate
   `/sys/bus/wmi/devices/` and treat each entry as a separate device.

9. **firmware-attributes drivers vary.** Dell, Lenovo, and HP each
   expose a different subset of types (Dell has `dell_modifier`;
   Lenovo has `save_settings` and `level`; HP has `ordered-list`).
   Zenctl should treat the class as documented and gracefully skip
   per-vendor attributes that don't exist.

10. **`/proc/acpi/event` removed.** Use the netlink `acpi_event`
    family. Zenctl's ACPI event listener must join `acpi_mc_group`.

11. **`/sys/class/wmi/` removed.** Use `/sys/bus/wmi/devices/`. Old
    code paths in the wild still reference `/sys/class/wmi/`.

12. **USB `power/level` and `power/autosuspend` deprecated.** Use
    `power/control` and `power/autosuspend_delay_ms` (note: ms, not
    s).

13. **`hciconfig` is deprecated.** Use `btmgmt`, `bluetoothctl`, or
    the kernel mgmt socket. Zenctl should not shell out to
    `hciconfig`.

14. **`/dev/bus/usb/<bus>/<dev>` permissions.** Default mode is
    `0664 root:root`. Without a `udev` rule placing the device in a
    writable group, non-root access fails with `EACCES`. Zenctl
    requires root for USBFS ioctls regardless.

15. **rfkill `state` file.** Marked as scheduled-for-removal in the
    ABI doc but still present. Zenctl should prefer `soft` + `hard`
    over `state`; treat `state` as a compatibility fallback only.
