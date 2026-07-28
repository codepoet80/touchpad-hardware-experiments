# btgamepad — Preware/WOSQI package for the Bluetooth gamepad stack

Packages the whole working setup (`libpmbtgamepad.so` + the system changes it
needs) into one installable `.ipk` for the **WOSA Modernize** feed.

## Build

```sh
./build-ipk.sh [version]        # default 1.0.0
# -> org.webosarchive.btgamepad_<version>_armv7.ipk
```
Builds the shim (`make` in the parent) if needed, assembles the ipk
(`debian-binary` + `control.tar.gz` + `data.tar.gz`, in that ar member order),
and prints the md5/size. Regenerate `Packages-stanza.txt` after any rebuild —
the md5 changes because `LastUpdated` is stamped at build time.

## What the package does (postinst, as root, idempotent)

1. installs `/usr/lib/libpmbtgamepad.so`
2. installs `/etc/udev/rules.d/99-bt-gamepad.rules` (pad node → 0666 for jailed apps)
3. bind-mounts `/dev/input` into the PDK jail (`/etc/jail_pdk.conf`)
4. patches the Bluetooth settings app (`models/Bluetooth.js` + `controllers/bluetooth-assistant.js`)
   so a gamepad paired from the **Other** category HID-connects (auto after
   pairing, on tap-to-reconnect, and device-initiated) — three edits that add
   `isGamepad`/`isMouse` to checks that stock gated on `isKeyboard` only. No
   `DeviceClass.js` change: the pad stays under Other where users expect it.
5. rewrites `/etc/event.d/bluetooth` to `LD_PRELOAD` the shim into PmBtEngine and
   force `sysrq=0` (dump logging **off** in this build: `WEBOS_BT_SHIM_DUMP=0`)

Every step backs up what it changes; **prerm restores all of it** (removal +
reboot = bit-for-bit stock Bluetooth).

### The reboot is declared, not forced

postinst does **not** reboot. The control's `Source` JSON carries
`"PostInstallFlags":"RestartDevice"` (and `PostUpdateFlags`), so Preware performs
the reboot and can **stack** it with other packages in the same batch. The
upstart-job change (step 5) only takes effect after that reboot.

## Install (WOSQI, manual)

WebOS Quick Install → Install package → pick the `.ipk`. WOSQI does not process
`PostInstallFlags`, so **reboot the device manually** after it installs. Then:
Bluetooth settings → Add device, put the pad in pairing mode, pair it from the
**Other** category (it connects automatically), and launch a gamepad-aware app.

## Add to the modernize feed

1. copy the `.ipk` into the feed's `ipkgs/`
2. append `Packages-stanza.txt` to `ipkgs/Packages` (keep existing stanzas verbatim)
3. regenerate `Packages.gz` (`gzip -n`, mtime 0) and `cmp` it against `Packages`

## Validated

- ipk structure (ar members, tar roots) — correct.
- v1.0.0 fresh-Preware install applied 5/6 changes; the 6th (BT-app patch) was
  silently skipped by a non-unique idempotency guard — fixed in 1.0.1, then the
  whole BT-app patch was redesigned in 1.1.0 (see below).
- 1.1.0 BT-app edits (3 seds) validated under **busybox sed** on-device: all
  three apply once, line counts unchanged, guards are idempotent; both patched
  files pass `node --check`.
- Source JSON parses (Preware-style `JSON.parse`).
- **Pending hardware confirmation:** first HID connect of a from-scratch-paired
  DS4 (needs the pad awake). Design connects without a radio cycle; verify.

## No-BLE caveat / controller scope

BT 2.1+EDR radio only — BLE controllers (Xbox One/Series, etc.) can't work.
The DS4 is covered by the shim's built-in descriptor; other classic-HID pads
work if they present a clean descriptor (the shim caches it) — grow the built-in
table as controllers are tested.
