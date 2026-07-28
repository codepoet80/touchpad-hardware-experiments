# bluetooth-shim

**Bluetooth gamepad + mouse support for the HP TouchPad (webOS 3.0.x), added by
interposing the closed-source Bluetooth stack — no binary patching.**

webOS's `PmBtEngine` happily *pairs and connects* any Bluetooth-HID device, but
only ever delivers **keyboard** input to the system. Mice and gamepads connect,
the stack even logs `[HIDH]: report from gamepad`, and then the report is thrown
away. This project restores mouse and gamepad input by `LD_PRELOAD`-interposing
the three HID→uinput bridge functions inside `libPmBtBsaif.so` and translating
reports properly, driven by each device's own HID report descriptor.

Target: legacy **2.6.35** webOS kernel (the shipped TouchPad kernel), where the
entire Bluetooth stack runs in userspace and `/dev/input/uinput` is the only
input-injection path. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the
full reverse-engineering writeup.

> **BLE?** No — and it can't be fixed in software. The TouchPad's radio is a
> **CSR BlueCore6-ROM (BC63B239A), Bluetooth 2.1+EDR**; it has no LE PHY, and the
> shipped CSR Synergy build has zero GATT/ATT/SMP/HOGP code. Only **classic
> BR/EDR** HID devices are in scope (which is what essentially all era-relevant
> BT mice and most gamepads use).

---

## Why input is dropped (the root cause)

All three findings were recovered from the shipped, unstripped binaries with
Ghidra + `objdump`. Full evidence in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### 1. The uinput node is keyboard-only

`PmBtBsaifHidOpenUInput` declares only `EV_KEY`:

```c
ioctl(fd, UI_SET_EVBIT, EV_KEY);            // 0x40045564, 1
for (i = 0; i < 0x100; i++)
    ioctl(fd, UI_SET_KEYBIT, i);            // all 256 keycodes
ioctl(fd, UI_SET_EVBIT, EV_MSC);
ioctl(fd, UI_SET_EVBIT, EV_REP);
ioctl(fd, UI_SET_MSCBIT, MSC_RAW);
// ... NO EV_REL, NO EV_ABS ...
write(fd, &uinput_user_dev, 0x45c);
ioctl(fd, UI_DEV_CREATE);                   // 0x5501
```

A node created without `EV_REL`/`EV_ABS` **cannot** carry a mouse motion or a
joystick axis — the kernel drops those events.

### 2. The translator only decodes boot keyboards

`PmBtBsaifHidSendToInput` switches on the report type, and for INPUT reports it
branches on a device-class field `dev[4]`:

```c
case 1:                                  // INPUT report
    switch (dev[4]) {
      case 2: log("...mouse...");             return;   // DROPPED
      case 3: log("[HIDH]: report from gamepad"); return;   // DROPPED
      case 1: /* boot-keyboard parse -> emits EV_KEY */ break;
      default: logError("unknown");           return;
    }
```

### 3. The classifier never sets the mouse/gamepad class

`PmBtBsaifHandleHidhPrim` only ever writes `dev[4] = 1` (keyboard, when the SDP
subclass bit `0x40` is set). Nothing in the whole library stores `2` or `3`, so
the mouse/gamepad arms above are effectively dead — a real gamepad reaches the
`default` drop. **Palm wrote the enum and the log string but never finished the
feature.**

## What the shim does

The bridge functions are exported with default visibility and are called through
the library's own PLT/GOT even internally, so `LD_PRELOAD` interposition
captures every call (including the one from `PmBtBsaifHandleHidhPrim`). The shim:

* **`PmBtBsaifHidOpenUInput`** — reads the device's HID report descriptor
  (which the stack stores at `dev+0x6a0`, length `dev+0x69e`), parses it, and:
  * for **keyboards** (or anything with no non-keyboard fields) delegates to the
    original function via `RTLD_NEXT` — stock behaviour is untouched;
  * for **mice / gamepads** creates its own uinput node advertising exactly the
    capabilities the descriptor needs (`EV_REL` for mice; `EV_ABS` axes + hat +
    `EV_KEY` `BTN_*` for pads), with correct axis ranges.
* **`PmBtBsaifHidSendToInput`** — for managed devices, decodes each report per
  the parsed field map and emits `EV_REL`/`EV_ABS`/`EV_KEY` + `EV_SYN`.
* **`PmBtBsaifHidCloseUInput`** — tears our node down (or delegates).

One generic, descriptor-driven translator therefore covers keyboards, mice and
gamepads. Devices appear as ordinary `/dev/input/event*` (+ `js*`) nodes.

## Build

Uses the Palm PDK toolchain (gcc 4.3.3, EABI soft-float — exact ABI match for
webOS 3.0.5). The shim does no floating-point math.

```sh
make               # -> libpmbtgamepad.so   (ARM, EABI5)
make host-test     # run the HID parser/emitter unit tests on the build host
```

Override the toolchain if needed: `make CROSS=/path/to/arm-none-linux-gnueabi-`.

## Deploy (novacom)

```sh
scripts/deploy.sh --dump     # install + enable descriptor/report logging
scripts/capture.sh           # tail /var/log/btshim.log; now pair a controller
# ... verify the descriptor + reports look right ...
scripts/deploy.sh            # reinstall without --dump for normal use
scripts/undeploy.sh          # fully revert
```

Deploy renames `/usr/bin/PmBtEngine` to `PmBtEngine.real` and installs a wrapper
that sets `LD_PRELOAD`; it is idempotent and reversible.

**Validate on-device before trusting the translator.** `--dump` logs each
device's report descriptor and every raw report. That both confirms the RE'd
struct offsets on real hardware and shows the exact report layout of your
specific controller. Then test:

```sh
cat /proc/bus/input/devices          # see the new event/js node
# use evtest / jstest against /dev/input/event* or /dev/input/js0
```

## Status

* HID descriptor parser + report→uinput translator: **implemented, unit-tested**
  (keyboard/mouse/gamepad classification, signed axes, buttons, 8-/4-way hat).
* On-device validation of struct offsets + real controller report layout:
  **pending first hardware run** (that is what `--dump` is for).

## Layout

```
src/shim.c        LD_PRELOAD interposers + managed-device table
src/hid_parser.c  HID report-descriptor parser -> field map + classification
src/uinput_dev.c  uinput node creation from a profile + report translation
src/devinst.h     RE'd device-instance / message struct offsets
src/compat.h      self-contained input/uinput ABI (no target headers needed)
test/             host-side unit tests (no device required)
docs/ARCHITECTURE.md   the full reverse-engineering map
scripts/          novacom deploy / capture / revert
```

## Safety

`undeploy.sh` restores the original binary and removes the shim. The shim never
modifies any shipped binary on disk — it only interposes at load time — so the
worst case is "Bluetooth input behaves exactly as stock" after reverting.
