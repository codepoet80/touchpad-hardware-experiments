# Distributing the Bluetooth gamepad stack as a Preware IPK

Plan for turning the working `webos-bt-shim` setup into something other TouchPad
owners can install through Preware / WebOS Quick Install. Everything below is
validated on our dev unit; none of the *packaging* is built yet.

---

## 1. What actually has to land on a user's device

The complete working state is six changes (see `DEVICE-STATE.md` #11–16 and the
`webos-bt-shim/scripts`). An installer must reproduce all of them:

| # | Change | File(s) | Needs reboot? |
|---|--------|---------|---------------|
| 1 | The shim | `/usr/lib/libpmbtgamepad.so` | no (loads on BT restart) |
| 2 | LD_PRELOAD the shim into PmBtEngine + `sysrq=0` | `/etc/event.d/bluetooth` (backup `/etc/bluetooth.upstart.btshim-orig`) | **yes** — upstart 0.3.x never re-parses a job at runtime |
| 3 | Gamepad node → `0666` for jailed apps | `/etc/udev/rules.d/99-bt-gamepad.rules` | no |
| 4 | Expose `/dev/input` in the PDK jail | `/etc/jail_pdk.conf` (backup `.btshim-orig`) | no (jails rebuild per launch) |
| 5 | Let mice/gamepads pair via the keyboard HID path | `com.palm.app.bluetoothtab` `DeviceClass.js` + `bluetooth-assistant.js` (backups `*.btshim-orig`) | no (relaunch the app) |
| 6 | Per-device: a valid `/var/hid.j` record | `/var/hid.j` | radio cycle |

Change #6 can't be fully pre-baked — it's created when the user pairs. The shim's
built-in descriptor fallback already covers the DS4 by VID/PID, so the user-facing
flow is just "pair, then press PS to reconnect." A shipped package should ship the
**flow + built-in descriptors**, not a canned `hid.j`.

The reboot for #2 is the one unavoidable install-time reboot. Everything else
applies live. This is normal for webOS Internals system patches — postinst prints
"reboot to finish."

---

## 2. Packaging mechanism — which Preware type

Preware distributes several kinds. Two are relevant:

- **Patches** — unified diffs against `/usr/palm/...` app files, applied/removed by
  the Preware patch engine. Perfect fit for change **#5** (the two JS edits) and
  nothing else — patches can't install binaries or touch `/etc`.
- **IPKGs installed via `org.webosinternals.ipkgservice`** — a normal `.ipk` whose
  `postinst`/`prerm` **run as root** and can do anything (remount `/` rw, drop the
  `.so`, edit `/etc`, install the udev rule). This is how every system tweak in the
  webOS Internals feeds ships. Fit for changes **#1–#4** (and #5 too, via `sed`, if
  we'd rather keep it all in one artifact).

**Recommendation: one self-contained IPKG that does all of #1–#5 in `postinst`.**
We already have working, idempotent, reversible shell for every step in
`webos-bt-shim/scripts/{deploy,undeploy,patch-bt-app,unpatch-bt-app}.sh` — the
package scripts are those, minus novacom (they run *on* the device). Skipping the
separate Preware-patch artifact keeps it to a single install/uninstall and one
thing for the user to find. (A Preware "Patch" for #5 is a nice-to-have v2 so the
Bluetooth-app change shows up in the Patches list and survives app reinstalls.)

### IPKG layout

```
org.webosinternals.btgamepad_1.0.0_all.ipk   (arch: arm / all)
├── control            id, version, maintainer, "type": leave app-less (system)
├── postinst           remount rw; install payload; patch /etc + app; reload udev;
│                      print REBOOT-REQUIRED
├── prerm              restore every backup; remove payload; reload udev
└── data/
    ├── usr/lib/libpmbtgamepad.so              (prebuilt ARM, from the PDK build)
    ├── etc/udev/rules.d/99-bt-gamepad.rules
    └── usr/palm/.../btgamepad/                 staging copies + the deploy logic
```

`postinst` is essentially `deploy.sh --setup` with the novacom `dev_sh` wrapper
removed. `prerm` is `undeploy.sh` + `unpatch-bt-app.sh`. Both are already written
and proven — porting them is mechanical.

### Open packaging questions to nail down first

1. **Does Preware's `ipkgservice` run `postinst` with `/` remounted rw?** It runs as
   root, but our scripts `mount -o remount,rw /` themselves, so this should be moot —
   confirm on-device with a trivial test ipk before building the real one.
2. **Root-fs space.** Payload is ~26 KB (`.so`) + a few tiny text files. `/` has room;
   confirm. If ever tight, stage under `/media/cryptofs` and symlink.
3. **App-file patch vs sed.** The `com.palm.app.bluetoothtab` JS differs between
   3.0.4 / 3.0.5. Our `sed` edits are anchored on stable substrings and are
   version-tolerant; a unified-diff patch is not. Prefer the sed approach in postinst.
4. **License to redistribute the shim.** Confirm `webos-bt-shim`'s license permits
   redistribution of the built `.so`. We ship **only our own** binary — no Palm
   binary is redistributed (the shim is a clean-room LD_PRELOAD interposer), so
   there's no Palm-IP issue, but the shim's own license must allow it. Coordinate
   with Herrie (upstream) before publishing.

---

## 3. Controller support breadth

The DS4 works today via a built-in descriptor keyed on `054c:05c4` / `054c:09cc`.
For a distributable, widen coverage:

- **Ship a built-in descriptor table** for the common BR/EDR (non-BLE) pads of the
  era: DualShock 4, DualShock 3 (needs USB pairing quirk — likely out of scope),
  8BitDo pads in DInput/“keyboard”… (verify which speak classic HID), generic
  “Wireless Controller” clones.
- **Descriptor recovery already generalises**: parse-as-delivered → u16-compaction →
  shim cache (`/var/btshim.d`) → built-in. Many pads that report a *clean* descriptor
  on first connect will work with no table entry at all (the shim caches it).
- **Hard wall: no BLE.** The TouchPad radio is BT 2.1+EDR — Xbox One/Series (BLE),
  and any BLE-only pad, cannot work. Document this prominently.
- Publish a short **tested-controllers matrix** in the package README and take
  community reports to grow it.

---

## 4. Staged plan

**MVP (ship the DS4, this is 90% done):**
1. Port `deploy.sh --setup` → `postinst`, `undeploy.sh`+`unpatch` → `prerm` (drop
   the novacom wrapper; they already run device-side shell).
2. Hand-build the `.ipk` (ar + two tar.gz + `debian-binary`, exactly like the game's
   `package-webos.sh`).
3. Smoke test the *install path itself*: `ipkg install` over novacom on a **clean**
   TouchPad (revert all our manual changes first, or use a second unit), reboot,
   pair a DS4, launch a gamepad-aware app.
4. Write a user README: pairing flow, the one reboot, "press PS to reconnect,"
   the no-BLE caveat, uninstall.

**v1 (make it a real Preware item):**
5. Split the Bluetooth-app JS change into a proper Preware **Patch** so it appears in
   the Patches list and re-applies after an app update.
6. Broaden the controller table; add the tested-controllers matrix.
7. Host a **feed** (GitHub Pages `Packages`/`Packages.gz` is enough for a Preware
   custom feed) + a GitHub Release with the raw `.ipk` for WebOS Quick Install.

**Stretch (polish):**
8. Replace the global `jail_pdk.conf` edit (exposes *all* input to *every* PDK app)
   with something tighter — ideally a per-app opt-in, or exposing only the gamepad
   node. Investigate whether a per-app `jail_app.conf` (the jailer already looks for
   `<appdir>/jail_app.conf` — it logged "not found") can add the bind-mount for just
   the game, so system-wide exposure isn't required.
9. A tiny root helper to auto-seed `hid.j` / drive pairing so the user doesn't touch
   the Bluetooth settings app at all.
10. Fold the shim fixes upstream into Herrie's repo so the package tracks upstream.

---

## 5. Security / honesty notes to put in the package description

- The jail change (#4) currently makes **all** `/dev/input` readable by **every** PDK
  app (touchscreen and any BT keyboard included). Acceptable for an opt-in homebrew
  tweak; call it out plainly, and pursue the per-app scoping in Stretch #8.
- `sysrq` is force-disabled at BT start (#2) — this is a *safety* improvement
  (prevents the KEY_SYSRQ panic), worth mentioning as a feature.
- Everything is reversible: `prerm` restores every backup, and the shim never
  modifies a shipped binary on disk (LD_PRELOAD only). Worst case after uninstall is
  bit-for-bit stock Bluetooth behaviour.
