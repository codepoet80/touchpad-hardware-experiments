# PmBtEngine HID-Host — Reverse-Engineering Map

This document captures the reverse engineering of the webOS Bluetooth HID-Host
path on the HP TouchPad, and explains exactly why Bluetooth **mice and gamepads**
pair but produce no input events, while keyboards work. It is the design basis for
the `webos-bt-shim` interposer.

All addresses are **file virtual addresses** in the shipped ARM binaries from
`nova-cust-image-topaz.rootfs-3.0.5` (webOS 3.0.5, "Topaz" = TouchPad). Binaries
are **not stripped** (`PmBtEngine.debug` carries full symbols — 2281 functions).
Decompilation was produced with Ghidra 12 headless.

---

## 1. Process & stack model (legacy 2.6.35 kernel)

On the legacy webOS kernel `CONFIG_BT` is **not set**. The entire Bluetooth stack
runs in **userspace** over a raw `/dev/bt_uart` character device:

```
  luna apps
     │  Luna service bus  (luna://com.palm.bluetooth/…)
     ▼
  PmBtEngine            ELF ARM, dynamically linked, NOT stripped
     │  profile/service logic:  Gap Hf Hfg A2dp Avrcp Spp Pbap Opp Mapc  Hidh
     │  links directly against ↓
     ▼
  libPmBtBsaif.so       CSR-Synergy "BSA" interface library.
     │                  Wraps the CsrBt* API, owns the HID→input bridge,
     │                  and — critically — the ONLY uinput code in the system.
     │  CsrPutMessage() IPC
     ▼
  PmBtStack             Full CSR Synergy stack (BCSP / HCI / L2CAP / SDP / HIDH …)
     │                  talking BCSP to the chip.
     ▼
  /dev/bt_uart  ──►  CSR BlueCore6-ROM  BC63B239A   (Bluetooth 2.1 + EDR)
```

Consequences that shape everything below:

* **No BLE.** BlueCore6 is a Bluetooth 2.1+EDR radio — there is no LE PHY. The
  Synergy build here has **no GATT / ATT / SMP / HOGP / advertising** symbols
  (the only `abcsp_le_*` symbols are BCSP *Link Establishment*; the only `…Att…`
  symbols are SDP *attributes*). BLE is a hardware wall, not a software gap.
* **No `uhid`.** `uhid` arrived in Linux 3.6. On 2.6.35 it does not exist.
* **No kernel `hidp`.** That needs `CONFIG_BT` (BlueZ), which is off, and would
  fight the userspace stack for the UART anyway.
* Therefore **`/dev/input/uinput` is the one and only input-injection path.**
  Any HID device that is to become a `/dev/input/event*` node must be translated
  to `input_event`s by userspace and written to uinput.

---

## 2. HID-Host message flow

Everything funnels through one dispatcher in `libPmBtBsaif.so`:

```
CSR HIDH primitive ──► PmBtBsaifHandleHidhPrim(msg)      @ 0x4a970  (~4.7 KB)
                          switch(prim->type):
   0x8000 CONNECT_IND
   0x8001 CONNECT_CFM ............ classify + PmBtBsaifHidOpenUInput()
   0x8002 DISCONNECT ............. PmBtBsaifHidCloseUInput()
   0x8006 DATA_IND .............. printOutHidData() + PmBtBsaifHidSendToInput()
   0x8007 CONNECT_ACCEPT_IND ..... store report descriptor + classify + OpenUInput()
   0x8008 …
```

* **`PmBtBsaifHidOpenUInput(dev, remdev)`** `@ 0x4c858` — opens `/dev/input/uinput`,
  declares capabilities, writes the `uinput_user_dev`, issues `UI_DEV_CREATE`.
* **`PmBtBsaifHidSendToInput(dev, msg)`** `@ 0x4cbe0` (~2.4 KB) — per-report
  translator; writes `input_event`s to the uinput fd.
* **`PmBtBsaifHidCloseUInput(dev)`** `@ 0x4c820` — `UI_DEV_DESTROY` + close.

All three are exported with **default visibility** and are called **through the
PLT/GOT even from inside the library** (stubs at `0x467f8` / `0x46900` / `0x4702c`
jump through GOT slots `0x…f373c` / `0x…f3794` / `0x…f39f8`). That is what makes
them **interposable via `LD_PRELOAD`** without patching the closed binary — see §6.

---

## 3. The uinput device is keyboard-only

Decompiled `PmBtBsaifHidOpenUInput` (Ghidra, lightly annotated). The ioctl request
numbers are the standard `linux/uinput.h` values (`_IOW(UINPUT_IOCTL_BASE, …)`,
`UINPUT_IOCTL_BASE='U'=0x55`, and `UI_SET_*` = `_IOW('U', 100+n, int)`
= `0x40045564 + n`):

```c
void PmBtBsaifHidOpenUInput(dev /*param_1*/, remdev /*param_2*/) {
  if (dev[0x118] != 0) return;                 // already open
  memset(dev + 0x120, 0, 0x45c);               // struct uinput_user_dev (1116 B)
  memset(dev + 0x57c, 0, 0x10);                // struct input_event scratch (16 B)
  fd = open("/dev/input/uinput", O_WRONLY|O_NONBLOCK);   // 1, 0x800
  dev[0x11c] = fd;
  ...
  strncpy(dev+0x120, remdev+0x12, 0x50);       // uinput_user_dev.name  (80)
  *(u16*)(dev+0x170) = 5;                       //   .id.bustype = BUS_BLUETOOTH
  *(u16*)(dev+0x172) = *(u16*)(remdev+4);       //   .id.vendor
  *(u16*)(dev+0x174) = *(u16*)(remdev+6);       //   .id.product
  *(u16*)(dev+0x176) = *(u16*)(remdev+8);       //   .id.version

  ioctl(fd, UI_SET_EVBIT, EV_KEY);              // 0x40045564, 1
  for (i = 0; i < 0x100; i++)
      ioctl(fd, UI_SET_KEYBIT, i);              // 0x40045565, 0..255  (all keycodes)
  ioctl(fd, UI_SET_EVBIT, EV_MSC);              // 0x40045564, 4
  ioctl(fd, UI_SET_EVBIT, EV_REP);              // 0x40045564, 0x14
  ioctl(fd, UI_SET_MSCBIT, MSC_RAW);            // 0x40045568, 3
  ioctl(fd, UI_SET_PROPBIT, remdev[0xb]);       // 0x4004556e
  write(fd, dev+0x120, 0x45c);                  // uinput_user_dev
  ioctl(fd, UI_DEV_CREATE);                     // 0x5501
  dev[0x118] = 1;
}
```

**Capabilities declared: `EV_KEY` (all 256 keycodes), `EV_MSC/MSC_RAW`, `EV_REP`.**
There is **no `UI_SET_EVBIT EV_REL`** and **no `UI_SET_EVBIT EV_ABS`**. A node
created this way physically cannot report a relative mouse motion or an absolute
joystick axis — the kernel drops any `EV_REL`/`EV_ABS` event on an uncapable node.

---

## 4. The translator only decodes boot keyboards

Decompiled `PmBtBsaifHidSendToInput`. `msg[3]` is the HID **report type**
(1 = INPUT, others = OUTPUT/FEATURE/other), and inside the INPUT case the code
branches on the **device class** stored at `dev[4]`:

```c
void PmBtBsaifHidSendToInput(dev /*param_1*/, msg /*param_2*/) {
  switch (msg[3]) {                      // report type
  case 0: log("Hid report - other");   break;
  case 2: log("Hid report - output");  break;    // dropped
  case 3: log("Hid report - feature"); break;    // dropped
  case 1:                                // ---- INPUT report ----
    switch (dev[4]) {                    // device class (see §5)
      case 2: log("...mouse...");            return;   // DROPPED
      case 3: log("[HIDH]: report from gamepad"); return;   // DROPPED
      case 1: /* keyboard — the only path that emits input_events */ break;
      default: logError("unknown");          return;
    }
    // boot-protocol keyboard parse:
    //  - report[2] modifier byte: 8 bits → 8 modifier keycodes via table
    //  - report[4..len] keycode array: press/release diff vs previous report
    //    (dev+0x14 = last report, dev+0x114 = last report len), rollover-safe,
    //    usage→keycode via the 256-byte `hid_keyboard` table @ 0xcaa30
    //  - each event: fill dev+0x584(type=EV_KEY) / +0x586(code) / +0x588(value),
    //    write(fd, dev+0x57c, 0x10); then write a SYN input_event.
    //  - one hard-coded consumer-remote special case (dev[0x590]==0x65)
    memcpy(dev+0x14, msg[8], msg[4]);    // save as "previous report"
    dev[0x114] = msg[4];
    break;
  }
}
```

The important message-field offsets, used by the shim:

| field | meaning |
|------:|---------|
| `msg[3]` (u8)  | report type — **1 = INPUT** |
| `msg[4]` (u16) | report length in bytes |
| `msg[8]` (ptr) | pointer to report bytes |

`input_event` here is 16 bytes (`struct timeval{sec,usec}` 8 + `type` 2 + `code` 2
+ `value` 4) — consistent with the `write(fd, …, 0x10)` calls.

---

## 5. Device classification — and why 2/3 never fire

Classification happens at connect time in `PmBtBsaifHandleHidhPrim`. Decompiled
`CONNECT_CFM` (0x8001):

```c
case 0x8001:
  if (ok) {
    if      (dev[0x58c] == 0)            log("Connect cfm - no sdpInfo");
    else if ((dev[0x596] & 0x40) == 0)  log("device is not a keyboard");   // <-- no dev[4] write
    else {                              log("device is a keyboard");
        CsrBtScEncryptionReq(...);
        dev[4] = 1;                     // <-- ONLY assignment to the class field
    }
    PmBtBsaifHidOpenUInput(dev, dev + 0x58c);
  }
```

`CONNECT_ACCEPT_IND` (0x8007) is where the **HID report descriptor is captured**.
`msg[0x14]` points at the parsed HID-info/SDP block; `msg[0x20]`/`msg[0x24]` are the
descriptor length/pointer:

```c
case 0x8007:
  info = msg[0x14];                       // parsed HID info (subclass @ info[8])
  if ((info[8] & 0x40) == 0) log("...not a keyboard...");
  else { log("...keyboard..."); dev[4] = 1; }
  ...
  // copy the HID SDP attribute header block into the message, then into dev+0x58c
  memcpy(dev+0x58c, msg+0x128, 0x514);    // dev+0x596 == subclass byte
  if (*(u16*)(msg+0x20) < 0x400) {        // descriptor length guard
      *(u16*)(dev+0x69e) = *(u16*)(msg+0x20);         // <-- descriptor length
      memcpy(dev+0x6a0, msg[0x24], *(u16*)(msg+0x20));// <-- RAW HID REPORT DESCRIPTOR
  } else logError("Hid descriptor length %d greater than allocated %d", …, 0x400);
  PmBtBsaifHidOpenUInput(dev, msg+0x128);
```

**Key finding:** `dev[4]` is written **only** with the value `1` (keyboard), and
only when subclass bit `0x40` is set. Nothing in the entire `libPmBtBsaif.so` ever
stores `2` or `3` there (verified: no `mov #2/#3 ; str [r,#4]` in the HIDH code).
So the `case 2` (mouse) and `case 3` (gamepad) arms of `SendToInput` are **dead** —
a real gamepad ends up with `dev[4] == 0` and falls into the `default`/"unknown"
drop. Palm wrote the gamepad log string and the class enum, but never wired the
classifier or the translator. **Mice and gamepads were never finished.**

### Device-instance struct offsets (RE-derived)

Given `dev` (the `local_24` device instance; `PmBtMemAlloc(0x63c)` ≈ 1596 bytes):

| offset | size | meaning |
|-------:|-----:|---------|
| `+0x02`   | u8   | device id / control handle |
| `+0x04`   | u32  | **device class** (1=keyboard; 0 otherwise — see above) |
| `+0x08`   | 6    | BD_ADDR |
| `+0x14`   | 256  | previous input report (for keyboard press/release diff) |
| `+0x114`  | u32  | previous report length |
| `+0x118`  | u8   | **uinput-created flag** |
| `+0x11c`  | int  | **uinput fd** |
| `+0x120`  | 1116 | **`struct uinput_user_dev`** (name@+0x120, id@+0x170) |
| `+0x57c`  | 16   | scratch `struct input_event` |
| `+0x584/6/8` | 2/2/4 | scratch event type / code / value |
| `+0x58c`  | 0x514 | copied HID-info block; `+0x590` vendor-ish, `+0x596` **subclass** |
| `+0x69e`  | u16  | **HID report-descriptor length** |
| `+0x6a0`  | ≤1024| **raw HID report descriptor** |

These are the offsets the shim reads. They are validated on-device by the shim's
dump mode (see README) before the translator trusts them.

---

## 6. Why LD_PRELOAD works here

`libPmBtBsaif.so` is a normal `DYN` object; the three bridge functions appear in
`.dynsym` with default visibility and each has a real `.plt` entry
(`PmBtBsaifHidSendToInput@plt` etc.). The internal caller `PmBtBsaifHandleHidhPrim`
reaches them through those PLT stubs / GOT slots rather than a direct
`bl <impl>` (the library was **not** built with `-Bsymbolic`/hidden visibility).

At load time the dynamic linker resolves each GOT slot to the first definition in
scope. An `LD_PRELOAD`ed object that exports the same symbol names therefore
**captures every call**, including the intra-library one from the dispatcher. The
shim uses `dlsym(RTLD_NEXT, …)` to reach the original implementations for the
keyboard path it deliberately leaves untouched.

---

## 7. Interposer strategy

The shim replaces the three bridge functions:

* **`PmBtBsaifHidOpenUInput(dev, remdev)`**
  * parse the descriptor at `dev+0x6a0` (len `dev+0x69e`) into a field map;
  * if the device is **keyboard-only**, delegate to the real function via
    `RTLD_NEXT` so existing behaviour is byte-for-byte preserved;
  * otherwise create our own uinput node advertising exactly the capabilities the
    descriptor needs (`EV_REL` REL_X/Y/WHEEL for mice; `EV_KEY` BTN_* + `EV_ABS`
    ABS_X/Y/RX/RY/Z/RZ/HAT0X/HAT0Y for pads), record `fd`→`dev+0x11c` and set the
    created flag, and remember the field map keyed by `dev`.
* **`PmBtBsaifHidSendToInput(dev, msg)`**
  * if `dev` is one we manage, decode `msg[8]`/`msg[4]` per the field map and emit
    `EV_REL`/`EV_ABS`/`EV_KEY` + `EV_SYN`;
  * otherwise delegate to the real function (keyboards, consumer remote).
* **`PmBtBsaifHidCloseUInput(dev)`** — tear down our node, or delegate.

An env-gated **dump mode** logs the descriptor and every raw report so the exact
byte layout of any specific controller can be confirmed on-device before/while the
generic parser is trusted.

---

## 8. Reproducing the decompilation

```
binaries: reports/bt-trace/webos-btbin/{PmBtEngine,PmBtEngine.debug,PmBtStack,
          libPmBtBsaif.so,libPmBtOs.so}

# families / symbols
nm --defined-only PmBtEngine.debug | grep Hidh
nm -D libPmBtBsaif.so | grep -iE 'Hidh|UInput|SendToInput'

# Ghidra headless decompile (Java post-script; PyGhidra not enabled in this build)
/opt/Ghidra/support/analyzeHeadless <proj> P -import libPmBtBsaif.so \
    -scriptPath <dir> -postScript Decomp.java -deleteProject
```
