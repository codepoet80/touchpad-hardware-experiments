# Bluetooth gamepad on webOS — what we learned, and what not to do

Status: **SOLVED (2026-07-27)** — via the LD_PRELOAD route this file's last
section recommended, implemented in Herrie's [webos-bt-shim](webos-bt-shim/)
(descriptor-driven interposer on libPmBtBsaif's HID→uinput bridge; branch
`ds4-hardware-bringup` adds report-framing auto-detect and a built-in DS4
descriptor fallback). A DualShock 4 now appears as a true gamepad evdev node —
14/14 buttons, both sticks, analog triggers, hat — with no keycode leakage, no
sysrq exposure, no sticking. The traps below remain valid history and still
apply to anything touching the stock keyboard path.

Read `README.md` for the mechanism. This file is about the traps.

---

## The one thing that clearly helped: a dedicated app that owns the connection

A Bluetooth pad arrives as a **keyboard**. Palm's stack runs its HID reports
through a boot-keyboard parser, so report bytes become keycodes. If the
**launcher** is focused when the pad connects, those keycodes drive the
launcher.

`btwizard/` implements the pattern that helped most: an SDL app goes fullscreen
**first**, then drives pairing and connection itself, so the controller is never
connected while the launcher is in focus. Keep this. Any game wanting controller
support should do the same — start the app, let the app establish the
connection, never pair from the launcher.

It is not sufficient on its own (focus theft still occurred — see open
questions), but it is a clear improvement and the right foundation.

---

## Do not do these things

### 1. Never retry `profconnect` in a loop
Only **7 HIDH sub-instances** exist and a failed connect consumes one
permanently (`CsrBtHidhConnectReqHandler` → result `0xB`, supplier 26). A
12-attempt loop exhausts them, after which **every** connect fails regardless of
hardware. This manufactures a convincing "the controller won't connect" fault
that survives re-pairing and looks like broken hardware.

**One `profconnect` per radio cycle.** A radio off/on frees the slots.

This also poisons experiments: a control run done after slot exhaustion is
meaningless, which is how we spent a long time believing a library patch caused
disconnects when the evidence was invalid.

### 2. Never map controller input onto system keycodes
When patching the library's usage→keycode table, keycodes must be chosen by
**construction from a safe pool**, never by "anything unused". A patch built the
lazy way put controller combinations on:

| combination | keycode | what it does |
|---|---|---|
| hat2 + Sq+X | 120 `SCALE` | **card view — steals focus** |
| hat6 + Tri | 143 `WAKEUP` | |
| hat3 + Tri | 139 `MENU` | |
| hat6 + Sq+O+Tri | 206 `CLOSE` | **closes the app** |
| hat7/8 + Sq+O+Tri | 207/208 `PLAY`/`FASTFORWARD` | audible honk |

This **hard-crashed the device** and produced what looked like the OS stealing
focus — it was the controller pressing "show card view" and "close app" itself.
Exclude at minimum: sysrq, power, sleep/wake, volume/mute, menu, close, scale,
browser and all media keys.

Letters and digits **are** safe when your app has focus — they only "type", and
nothing is listening.

### 3. Never leave SysRq enabled
The uinput keyboard Palm creates has the **sysrq handler attached**
(`Handlers=sysrq diag kbd eventN`). Report bytes land on `KEY_SYSRQ`, after
which further bytes are kernel commands — including *Crash*. This panicked the
device (`lastboot=panic`, previous klog full of `SysRq : HELP`).

```sh
echo 0 > /proc/sys/kernel/sysrq
```
Persistent job at `/etc/event.d/no-sysrq`. It reads 0 on a fresh boot but was
non-zero during a session, so something enables it at runtime — do not assume.

### 4. Do not reassign HID usages `0xe0`–`0xe7`
Those are the modifier slots, and their keycodes are how report **byte 0** (the
one recoverable analog axis) is transmitted. They also look like legal button
values, so naive table patching silently destroys the stick.

### 5. `subClass: 128` (pointing device) is a dead end
The library's uinput setup never calls `UI_SET_RELBIT`, so the device it creates
cannot carry relative axes at all, and every report is dropped with
`unknown hidDevType 0x0`. **64 is the only value that works.**

### 6. Do not assume `radioon` succeeded
It intermittently returns `Message status unknown` and leaves the radio off,
which strands any app waiting for a connection that can never arrive. Verify by
checking that `PmBtStack`/`PmBtEngine` are running, and retry.

### 7. `/var/hid.j` is read at stack startup
Editing `subClass` has no effect until a radio cycle. Editing it and immediately
reconnecting yields result `0x7` / "no sdpInfo". Correct order:
pair → **one** connect (this writes hid.j) → patch subClass to 64 → radio cycle
→ device-initiated reconnect (press PS).

---

## Decoding: two approaches tried, both insufficient

The channel is lossy by construction: a 10-byte controller report is squeezed
into an 8-byte boot-keyboard report. Byte 0 survives as the modifier bitmask
(one clean analog axis). Bytes 2–7 arrive as an unordered set of keycodes with
no indication of which slot they came from.

**Event tracking** (follow press/release, maintain the current button byte):
wedges permanently if a single release is missed. Every variant — nearest-match,
release-gating, batch resolution, awaiting-replacement — eventually latched.

**State polling** (`EVIOCGKEY` each frame): cannot wedge in principle, and fixes
initialisation (evdev only reports *changes*, so attaching to a resting pad
never learns its state). But in practice it was **worse** — buttons stuck
harder. The likely reason, worth verifying: the parser appears to leak key-down
without a matching key-up, so the kernel believes many keys are simultaneously
held. If true, neither events nor state are trustworthy from this channel.

Also structural: d-pad **up** is untransmittable. Its button byte is `0x00`,
which is the HID "empty slot" marker, so the parser discards it before any table
lookup. It can only be inferred from a release with no replacement — which is
exactly the fragile bookkeeping that wedges.

---

## Open questions for the next attempt

1. **What actually steals focus?** It still happened with a patch containing no
   system keycodes. `PDL_BannerMessagesEnable(PDL_FALSE)` did not stop it. Is it
   a notification type banners don't cover, or something else entirely? A
   shell-launched binary cannot re-assert focus; packaging as a real `.ipk`
   would let it call `applicationManager` to take focus back.
2. **Does the parser leak key-down without key-up?** Testable: connect, press
   nothing, and dump `EVIOCGKEY` — if keys read as held with the pad at rest,
   this channel cannot be decoded reliably by any means and the effort should go
   to the `LD_PRELOAD` route instead.
3. **Why did one session hold for 30+ minutes** while later ones dropped in
   seconds? Slot exhaustion explains some of it, not all.

## The route that avoids all of this

`LD_PRELOAD` a shim into PmBtEngine hooking **`PmBtOsMemCpy`**, which
`HandleHidhDataInd` calls first thing as `memcpy(dst, msg+0x120, 6)`. From
`src-0x120` you recover the message, whose **untouched 10-byte report** sits at
`msg+0x12c` (see `touchpad-bt-hid-internals` in memory).

That single hook removes every problem above at once: no lossy keycode table, no
sticking, no sysrq exposure, no system-key collisions, no hidd injection, and it
unlocks the second stick and analog triggers this channel structurally cannot
carry. It is the only approach where the data path is not fighting the platform.
