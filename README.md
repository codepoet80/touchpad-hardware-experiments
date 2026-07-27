# webOS hardware tests — USB and Bluetooth accessories on the HP TouchPad

Findings and tools from making modern accessories work on a stock webOS 3.0.5
TouchPad. See `DEVICE-STATE.md` for the exact state of the dev unit and how to
revert each change.

## The short version

The TouchPad's kernel is far more capable than webOS ever admitted. USB host
mode works (keyboards, mice, gamepads, mass storage), it sources bus power, and
the drivers are all built in. What is missing is *userspace*: nothing above the
kernel consumes non-keyboard input, and nothing auto-mounts a disk.

| Accessory | Kernel | webOS UI | Notes |
|---|---|---|---|
| USB keyboard | works | **works** | the one class Palm wired up end to end |
| USB gamepad | works (evdev) | via `padkeys` | analog + digital, DS4 included |
| USB mouse | works (evdev) | no | no mousedev, no cursor concept in webOS |
| USB mass storage | works | no | mount by hand, full VFAT read/write |
| Bluetooth keyboard | n/a | works | supported path, via Palm's stack |
| Bluetooth gamepad/mouse | — | **no** | see "Bluetooth" below — not fixable in userspace |
| Bluetooth LE anything | — | no | 2011 stack predates BLE |

## Tools

Both are static ARM binaries; build with
`arm-linux-gnueabi-gcc -static -O2 -march=armv7-a -o <name> <name>.c`
and are installed on the dev unit at `/usr/local/bin/`.

### padkeys — gamepad to keyboard shim

The useful one. webOS ignores `BTN_*`/`ABS_*` events but consumes keyboards
fully, so `padkeys` reads any evdev gamepad, translates to `KEY_*` codes, and
injects them through `/dev/input/uinput` as a virtual keyboard. hidd's
`HidInputDev` plugin inotify-watches `/dev/input`, picks the virtual device up
like a real one, and events flow to every app — including old PDK games that
only ever supported the Bluetooth keyboard.

```
padkeys [seconds]      # 0 = run until killed, default 300
```

| Control | Key |
|---|---|
| D-pad / left stick | arrows |
| Button 1 / Cross | Enter |
| Button 2 / Triangle | Space |
| Buttons 3–4 / Square | Z, X |
| Shoulders L1/R1 | A, S |
| Triggers L2/R2 | Q, W |
| Circle / PS / Select | Esc, Tab |

Remap by editing the `map_120[]` (classic joystick button range) and
`map_130[]` (gamepad range, e.g. DualShock 4) tables.

### padview — live controller visualizer

Draws controller state straight to `/dev/fb0` over whatever webOS is showing:
sticks as dots in boxes, triggers as bars, d-pad arrows, button grid. Useful
for confirming a pad works and for eyeballing analog fidelity and latency.

```
padview [seconds] [/dev/input/eventN ...]   # no device args = autodetect
```

## Bluetooth

There is no kernel Bluetooth at all (no `CONFIG_BT`, no BlueZ, no hci tools).
Palm ships a closed userspace CSR Synergy stack — `PmBtStack` + `PmBtEngine`
(`com.palm.bluetooth`) — driven entirely over the Luna bus. Radio control,
discovery, and pairing are all scriptable; recipes are in `DEVICE-STATE.md`.
The Settings app's `Bluetooth.js` is the API rosetta stone.

**Gamepads do not work over Bluetooth on stock software, and there are two
stacked blockers**, established by disassembly (both binaries are unstripped)
plus live tracing with the stack's debug zones enabled:

1. **The HID link never establishes.** With a valid fresh pairing, every
   `HidhConnect` fails with `Connect cfm error with code 0xB` followed by
   `Connect cfm - no sdpInfo`; the stack cannot obtain the DualShock 4's SDP
   record, so no HID device instance is created, which is what produces the
   "invalid device ID" noise and the endless reconnect loop. Waking the pad and
   pressing PS makes no difference.
2. **Even if it connected, the reports would be dropped.** `libPmBtBsaif.so`
   (which runs inside PmBtEngine) contains a complete HID-to-uinput injector —
   it opens `/dev/input/uinput` and carries a 256-byte usage-to-keycode table —
   but it is a boot-protocol host that parses only keyboard and mouse reports
   and sends anything else to an "unhandled report" branch. Separately,
   PmBtEngine's `HandleHidhDataInd` string-compares every device name against
   the literal `"HP TouchPad Wireless Keyboard"` and frees the buffer on
   mismatch.

So this is not a weekend `LD_PRELOAD` job. Blocker 1 has to be understood
first, and the way to do that is the stack's own built-in HCI sniffer
(`palmsniffer`, port in `/var/log/btport`, output to `/var/log/palmsniffer.log`)
to see whether an SDP query is even sent and what the pad answers. Only then
does the report path matter — and at that point `padkeys` is already the
consumer, so gamepads would light up system-wide with no further work.

A trap worth knowing: pairings can silently vanish. A trusted-device list that
included the DS4 was empty minutes later, and the resulting connect failures
look exactly like a HID-class rejection when they only mean "not paired".
Re-check `gap/gettrusteddevices` before believing any connect trace.

**A classic Bluetooth mouse may well work as-is** — the library handles mouse
reports through the same uinput path. Untested, but it is free if true. BLE
devices are invisible regardless; the 2011 stack predates BLE.

## Wireless novacom

Testing accessories needs the TouchPad's only USB port, so novacom runs over
Wi-Fi instead. That required a fix to novacomd's TCP transport, upstreamed to
[webOSArchive/webos-sdk-redux](https://github.com/webOSArchive/webos-sdk-redux)
along with `NOVACOM-TCP.md` documenting the bug and the full host/device setup.
