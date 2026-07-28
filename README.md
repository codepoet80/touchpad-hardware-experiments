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
| Bluetooth gamepad | — | **works, fully** | real evdev gamepad node via `bluetooth-shim/` — all buttons, sticks, analog triggers |
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

There is no kernel Bluetooth in the shipping config (`# CONFIG_BT is not set`),
though modules can be built and loaded — see `BLUETOOTH-KERNEL.md`.
Palm ships a closed userspace CSR Synergy stack — `PmBtStack` + `PmBtEngine`
(`com.palm.bluetooth`) — driven entirely over the Luna bus. Radio control,
discovery, and pairing are all scriptable; recipes are in `DEVICE-STATE.md`.
The Settings app's `Bluetooth.js` is the API rosetta stone.

### SOLVED PROPERLY: bluetooth-shim

The real fix is **[`bluetooth-shim/`](bluetooth-shim/)**, an LD_PRELOAD
interposer on `libPmBtBsaif.so`'s HID→uinput bridge. It began from the original
interposer concept by **Herrie** (webOS community), which we carried the rest of
the way: report-framing fix, descriptor-cache recovery, the PDK-jail and udev
work, the Bluetooth settings-app connect edits, and a deployable Preware package.
A DualShock 4 now appears as a true gamepad evdev node — **14/14 buttons, both
sticks, both analog triggers, and the d-pad hat, all verified on hardware** —
with no keycode leakage into the UI and no sysrq exposure (the node has no kbd
handler at all). Key hardware findings, documented in `bluetooth-shim/`:

* Reports arrive framed `[0xA1][report-id][payload]` — decoders must skip the
  HIDP header (Palm's own boot-keyboard parser confirms this framing).
* The `/var/hid.j` descriptor cache is irreparably corrupt by design (written
  as unpadded `%x`, read back with hex letters zeroed) — the shim recovers via
  a built-in per-VID/PID descriptor. Keep a copy of the DS4 record
  (`bluetooth-shim/docs/ds4-hid.j-record.json`): unpairing deletes `/var/hid.j`,
  and `profconnect` fails `no sdpInfo` without it (the incoming-pairing popup
  never runs the HID SDP query).
* `PmBtEngine` dies mid-session if the display sleeps (suspend churn) — any
  fullscreen app or keepawake prevents it, and it self-heals regardless
  (respawn → pad auto-reconnect → shim re-takeover).
* Reconnect is just the PS button once `hid.j` + link key exist.

Everything below this point documents the **earlier keyboard-channel route**,
kept as history — it still applies to anything touching the stock keyboard
path, but new work should use the shim's evdev node instead.

**Bluetooth gamepads DO work** — solved 2026-07-27, with no binary patching.
Two separate problems, both fixable from userspace:

1. **Connecting.** Earlier failures were a sleeping controller (HCI `Page Timeout
   0x04`) plus a stale HIDH user-slot registration after a radio off/on (CSR
   result `0xB`). The working recipe: clean radio off→on, then scan → `gap/pair`
   (auto-accepting `notifnsspjustworks` via `gap/ssppairaccept`) → **connect
   immediately**, while the registration is fresh and the pad is still awake.
   Connects on the first attempt.

2. **Reports being dropped.** `libPmBtBsaif.so` *does* create a uinput device for
   the pad, but every report died with `unknown hidDevType 0x0` because it only
   dispatches keyboard and mouse. The device type comes from the SDP subclass
   cached in `/var/hid.j`, which is **plain JSON**:

   ```json
   "sdp":{"isValid":1,"vendorId":1356,"productId":1476,"subClass":8,...}
   ```

   `subClass: 8` decodes as "neither keyboard nor pointing / gamepad". Set it to
   **64** (the keyboard bit), then reconnect *device-initiated* (press PS) so the
   stack uses the cached record instead of re-querying SDP. The errors vanish and
   key events flow into webOS.

**What those events are:** the library parses the DS4's report bytes as a HID
boot-protocol keyboard report — byte 0 becomes the modifier bitmask, bytes 2-7
become keycodes. That is lossy but *reversible*: the eight modifier keys are the
bits of report byte 0, and the other keycodes are the literal values of bytes
2-7. Since a DS4 report is `[reportID, LX, LY, RX, RY, buttons...]`, those bytes
carry both analog sticks and the button state, so full gamepad state can be
reconstructed - analog included.

> ### ⚠️ Disable SysRq before connecting a Bluetooth gamepad
> The uinput keyboard Palm's stack creates has the **sysrq handler attached**
> (`Handlers=sysrq diag kbd eventN`). A gamepad's report bytes land on
> `KEY_SYSRQ`, and once that is held the following bytes become kernel
> commands — including *Crash*, *terminate-all-tasks* and *kill-all-tasks*.
> This hard-panicked the test device (`lastboot=panic`, previous kernel log
> full of `SysRq : HELP`), and also explains apps closing "randomly".
>
> ```sh
> echo 0 > /proc/sys/kernel/sysrq
> ```
> A persistent job is installed at `/etc/event.d/no-sysrq`. Note sysrq reads 0
> on a fresh boot but was non-zero during our session, so something enables it
> at runtime — do not assume it is safe.

### The known-good configuration

**Stock `libPmBtBsaif.so` + `subClass: 64`.** In that state a DualShock 4 stayed
connected through 30+ minutes of continuous testing and delivered decodable
input the whole time (12,000+ captured events). Return to this baseline before
trying anything else.

Order matters, because `ConnectAcceptReq` needs the cached SDP record and the
stack reads `/var/hid.j` **at startup**:

1. Pair (scan → `gap/pair` → auto-accept `notifnsspjustworks`).
2. **One** `prof/profconnect`. This is what writes `/var/hid.j`.
3. Patch it: `sed -i 's/"subClass":8/"subClass":64/' /var/hid.j`
4. Radio cycle, so the stack loads the patched record.
5. Press PS on the controller — a device-initiated reconnect uses the cache
   instead of re-querying SDP.

> **Never retry `profconnect` in a loop.** Only **7** HIDH sub-instances exist
> and a failed connect consumes one permanently (result `0xB`, supplier 26).
> A retry loop exhausts them and then *every* connect fails regardless of the
> hardware — it manufactures a "won't connect" fault that looks like a hardware
> problem. One attempt per radio cycle.

### The noise problem is an app-focus problem — let the app drive the connection

A Bluetooth pad arrives as a *keyboard*, so its report bytes become keycodes. If
the **launcher** is the focused window when the controller connects, those
keycodes drive the launcher: volume jumps, apps open and close. `EVIOCGRAB` does
not fix this — Palm's stack also injects into hidd's private sockets
(`/var/run/hidd/KeypadEventSocket`), and it destroys and recreates the uinput
device on every reconnect, silently invalidating any existing grab.

**The fix is structural: the app takes the screen first, then establishes the
connection.** `bt-wizard/` implements it — an SDL app that goes fullscreen, walks
the user through pairing, patches and reloads the cached record, prompts for the
PS button, then grabs and decodes. Because the app is already focused, the
launcher never sees a single keycode. There is no race to win: the app was there
before the device existed.

**Any game wanting controller support should follow this shape** — start the app,
let the app drive pairing and connection, never pair from the launcher. It also
forces `sysrq` off at startup, and returns to the "press PS" screen and re-grabs
if the controller drops.

**Patching the library's lookup table is not recommended.** Rewriting the
256-byte HID-usage→keycode table at file offset `0xcaa30` in `libPmBtBsaif.so`
does silence the harmful keycodes at source, and it is an appealing idea — but
HID disconnects began immediately after installing that patch and had never
occurred before it. Treat it as harmful until someone proves otherwise against a
clean baseline. The original is kept on the device at
`/usr/lib/libPmBtBsaif.so.orig`.

`subClass: 128` (pointing device) is a dead end: the library's uinput setup never
calls `UI_SET_RELBIT`, so the device it creates cannot carry relative axes at
all, and the dispatch drops every report with `unknown hidDevType 0x0`. **64 is
the only value that works.**

So this channel gives one clean analog axis (report byte 0, via the modifier
bits) plus the button byte (d-pad hat and four face buttons). The second stick
and the analog triggers live in bytes the usage→keycode table folds onto
`KEY_UNKNOWN`. Getting a whole controller means an `LD_PRELOAD` shim on
PmBtEngine hooking `PmBtOsMemCpy` — called at the top of `HandleHidhDataInd`
with the message whose untouched report buffer sits at offset `0x12c` — which
also sidesteps the hidd injection entirely, since the report is consumed before
Palm's parser sees it.

**The UI must be SDL, not the framebuffer.** `padview` drew to `/dev/fb0` and
flickered continuously: the TouchPad has a 3-layer compositor and SDL is the only
sanctioned context owner. `padview-sdl.c` renders correctly and sits still. Build
against `/opt/PalmPDK` with the Linaro 4.9.4 toolchain (modern GCC links against
glibc symbols the device lacks), and run it as root from a novacom shell — the
launcher jails PDK apps as uid 5003, which cannot open `/dev/input/event*`.

**A classic Bluetooth mouse may well work as-is** — the library handles mouse
reports through the same uinput path. Untested, but it is free if true. BLE
devices are invisible regardless; the 2011 stack predates BLE.

## Wireless novacom

Testing accessories needs the TouchPad's only USB port, so novacom runs over
Wi-Fi instead. That required a fix to novacomd's TCP transport, upstreamed to
[webOSArchive/webos-sdk-redux](https://github.com/webOSArchive/webos-sdk-redux)
along with `NOVACOM-TCP.md` documenting the bug and the full host/device setup.
