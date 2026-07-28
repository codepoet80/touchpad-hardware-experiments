#!/usr/bin/env bash
#
# patch-bt-app.sh -- patch the stock webOS Bluetooth settings app so paired
# GAMEPADS (and mice) connect over HID. Stock already lists a gamepad under the
# "Other" category and types it DEVICETYPE='Gamepad', but three checks only ever
# act on keyboards, so it never HID-connects. Three edits fix it:
#   (a) models/Bluetooth.js connectHid(): accept gamepad/mouse, not just keyboard
#       -> enables auto-connect right after pairing
#   (b) controllers/bluetooth-assistant.js tap handler: let a disconnected
#       Gamepad/Mouse fall through to connect (tap-to-reconnect)
#   (c) models/Bluetooth.js inbound reconnect: accept gamepad/mouse
#       -> press the pad's button to reconnect (device-initiated)
# Backups: *.btshim-orig next to each file. Idempotent (guards key on the
# patched result). unpatch-bt-app.sh reverts.
#
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
A=/usr/palm/applications/com.palm.app.bluetoothtab/app
dev_sh() { $NOVACOM run file://bin/sh; }

{
  echo 'set -e'
  echo 'mount -o remount,rw / >/dev/null 2>&1'
  echo "M=$A/models/Bluetooth.js; C=$A/controllers/bluetooth-assistant.js"
  echo '[ -f "$M.btshim-orig" ] || cp "$M" "$M.btshim-orig"'
  echo '[ -f "$C.btshim-orig" ] || cp "$C" "$C.btshim-orig"'
  echo 'grep -q "isKeyboard(device.cod) || isGamepad" "$M" || sed -i '"'"'s/if( isKeyboard(device.cod)) {/if( isKeyboard(device.cod) || isGamepad(device.cod) || isMouse(device.cod)) {/'"'"' "$M"'
  echo 'grep -q "isGamepad(this.deviceCoD" "$M" || sed -i '"'"'s/if(isKeyboard(this.deviceCoD\[payload.address\])){/if(isKeyboard(this.deviceCoD[payload.address]) || isGamepad(this.deviceCoD[payload.address]) || isMouse(this.deviceCoD[payload.address])){/'"'"' "$M"'
  echo 'grep -q '"'"'DEVICETYPE != "Gamepad"'"'"' "$C" || sed -i '"'"'s/!= "Audio" \&\& device.DEVICETYPE != "Phone" \&\& /!= "Audio" \&\& device.DEVICETYPE != "Phone" \&\& device.DEVICETYPE != "Gamepad" \&\& device.DEVICETYPE != "Mouse" \&\& /'"'"' "$C"'
  echo 'echo "--- verify (each should print 1) ---"'
  echo 'grep -c "isKeyboard(device.cod) || isGamepad" "$M"'
  echo 'grep -c "isGamepad(this.deviceCoD" "$M"'
  echo 'grep -c '"'"'DEVICETYPE != "Gamepad"'"'"' "$C"'
} | dev_sh

echo ">> patched.  Fully close the Bluetooth app (swipe the card away) and reopen it"
echo "   so the changes load.  Pair a gamepad from the 'Other' category; it connects."
