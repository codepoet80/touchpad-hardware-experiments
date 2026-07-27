# TouchPad dev unit — modification inventory

Unit: HP TouchPad "topaz-linux", webOS 3.0.5, nduid `c37f7a3418b2688e6d933844b48ba803061b4fac`,
Wi-Fi MAC `00:1D:FE:DA:9B:81`, IP `192.168.10.67` (DHCP — reservation recommended).
Last updated: 2026-07-27.

## Changes on the device

| # | Change | Persistent? | Rollback |
|---|--------|-------------|----------|
| 1 | `/etc/event.d/novacomd`: exec line changed to `/sbin/novacomd -b` (novacom listens on all interfaces: 6968 devlist, 6969 inet transport, 6970 log) | Yes (applies at boot) | `cp /etc/novacomd.upstart.bak-preb /etc/event.d/novacomd` + reboot |
| 2 | `/etc/event.d/novacom-wifi-fw`: new upstart job; 60s after novacomd starts, inserts `iptables -I INPUT 1 -i eth0 -s 192.168.10.0/24 -p tcp --dport 6968:6970 -j ACCEPT` | Yes | `rm /etc/event.d/novacom-wifi-fw` + reboot |
| 3 | Runtime iptables ACCEPT rule for 6968–6970 from 192.168.10.0/24 on eth0 | No (re-created at boot by #2) | `iptables -D INPUT -i eth0 -s 192.168.10.0/24 -p tcp --dport 6968:6970 -j ACCEPT` |
| 4 | USB OTG forced to host mode: `echo host > /sys/kernel/debug/otg/mode` | No (reverts at reboot) | `echo peripheral > /sys/kernel/debug/otg/mode` |
| 5 | Backup file `/etc/novacomd.upstart.bak-preb` (original novacomd job) | Yes | delete after #1 is reverted |
| 6 | `/usr/local/bin/padkeys` and `/usr/local/bin/padview` installed (see README) | Yes | `rm /usr/local/bin/padkeys /usr/local/bin/padview` |
| 7 | Bluetooth: DS4 pairing removed, radio turned off, debug zones reset — **device left as found** | n/a | re-enable with `btmonitor/monitor/radioon` |
| 8 | `/var/log/bt.log` contains this session's HID debug trace | Yes (log rotation clears it) | `rm /var/log/bt.log` |

Notes:
- While in host mode (#4): no charging via the USB port, no novacom-over-USB, no USB drive
  mode — the Wi-Fi novacom link is the only access path. Touchstone charging still works.
- Never kill/stop novacomd from a novacom session (upstart won't respawn on TERM → total
  lockout until physical reboot). Never leave backup copies inside `/etc/event.d/` (every
  file there is treated as a job and races the real one at boot).
- Network novacom is an unauthenticated root shell, scoped by #2 to the 192.168.10.0/24 LAN.

## Changes on the host (AtlasL)

| # | Change | Rollback |
|---|--------|----------|
| 1 | `/usr/local/bin/novacomd` replaced with build from patched [webos-sdk-redux](https://github.com/webOSArchive/webos-sdk-redux) (SO_RCVLOWAT→MSG_WAITALL fix, commit `06979cd`; doc in `NOVACOM-TCP.md`) | `sudo cp /usr/local/bin/novacomd.bak-rcvlowat /usr/local/bin/novacomd && sudo systemctl restart novacomd` |
| 2 | systemd drop-in `/etc/systemd/system/novacomd.service.d/tcp-device.conf`: `ExecStart=/usr/local/bin/novacomd -c 192.168.10.67:6969` | delete the drop-in, `sudo systemctl daemon-reload && sudo systemctl restart novacomd` |

## Current state / usage

- `novacom -l` shows the device as `tcp emulator` (plus `usb topaz-linux` when cabled).
  Target the Wi-Fi path with `novacom -d tcp …`.
- USB host mode verified working: EHCI + HID built into the stock kernel, VBUS is sourced
  by the tablet, Apple Extended USB Keyboard (with internal 3-port hub) enumerates and
  types into the webOS UI.

## USB host-mode findings (accessory testing)

- **Interrupt transfers can wedge; cycle the controller to fix.** (CONFIRMED both ways:
  the same low-speed gamepad that was silent when directly attached to a churned
  controller delivered 534 events directly attached to a clean one; a low-speed mouse
  also worked directly. An interim "low-speed devices need a hub" conclusion was wrong —
  attachment position and device speed never mattered.) Symptom: device enumerates fine
  but its interrupt endpoint delivers nothing (usbmon shows zero traffic, no errors).
  Cause: prolonged failed-suspend churn wedges the EHCI periodic schedule while control
  transfers keep working. Fix is the same OTG cycle as the IRQ storm below. Verified
  data: Logitech Precision Gamepad 046d:c21a (8 buttons BTN 0x120–0x127, d-pad as
  ABS_X/ABS_Y 0/128/255), Logitech Optical Mouse 046d:c077 (REL_X/REL_Y, 3 buttons,
  wheel — evdev only, webOS has no mousedev/cursor).
- **IRQ storm on unplug.** Unplugging in forced host mode can storm the shared OTG/EHCI
  interrupt until the kernel disables IRQ 132, deafening all USB (no hotplug, stale
  sysfs). Recover without reboot:
  `echo peripheral > /sys/kernel/debug/otg/mode; sleep 4; echo host > /sys/kernel/debug/otg/mode`
- **Autosleep interferes with testing.** Screen-off triggers suspend attempts every
  ~2.5 s (each fails on USB with error -16 and thaws again). powerd activityStart
  renewals did not reliably stop it. Make on-device tests timing-insensitive
  (nohup'd captures to /tmp, pull results later over Wi-Fi novacom).
- **No joydev in the kernel** — gamepads are evdev-only (`/dev/input/eventN`), and the
  webOS UI ignores non-keyboard input devices; games must read evdev directly.
- **High-draw devices are software-rejected, and the override works.** The root port
  budgets ~390 mA; a DS4 controller (054c:05c4, declares 500 mA) enumerates but gets
  "rejected 1 configuration due to insufficient available bus power" — no HID, no input
  node. Override: `echo 1 > /sys/bus/usb/devices/1-1/bConfigurationValue` (bypasses the
  budget check; kernel logs the excess and configures anyway). Must be repeated on each
  replug. Once configured, the DS4 is fully functional through generic HID: both analog
  sticks 0–255 (ABS_X/Y + ABS_Z/RZ), analog triggers (ABS_RX/RY), d-pad as HAT0X/Y,
  all buttons. Hardware caveat: the tablet really is sourcing that current — if the
  connection gets flaky, feed the Y-cable's power leg from a charger.
- **USB mass storage works out of the box**: usb-storage/sd/VFAT all built in. Kingston
  DT 101 G2 (high-speed, 480M) auto-attached as /dev/sda1; manual
  `mount -t vfat -o utf8 /dev/sda1 <dir>` gives full r/w. Nothing auto-mounts and the
  webOS UI is unaware — mount/umount by hand (umount + sync before unplugging).
- Keepawake loop, if left running, is stopped with: `touch /tmp/stop-awake` (device).
