#!/usr/bin/env bash
# unpatch-bt-app.sh -- restore the stock Bluetooth settings app (model + assistant,
# plus any DeviceClass.js backup left by the pre-1.1.0 approach).
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
A=/usr/palm/applications/com.palm.app.bluetoothtab/app
dev_sh() { $NOVACOM run file://bin/sh; }
{
  echo 'mount -o remount,rw / >/dev/null 2>&1'
  echo "M=$A/models/Bluetooth.js; C=$A/controllers/bluetooth-assistant.js; DC=$A/controllers/DeviceClass.js"
  echo '[ -f "$M.btshim-orig" ] && mv -f "$M.btshim-orig" "$M" && echo model-restored || echo "model: no backup"'
  echo '[ -f "$C.btshim-orig" ] && mv -f "$C.btshim-orig" "$C" && echo assistant-restored || echo "assistant: no backup"'
  echo '[ -f "$DC.btshim-orig" ] && mv -f "$DC.btshim-orig" "$DC" && echo deviceclass-restored || true'
} | dev_sh
echo ">> restored.  Close and reopen the Bluetooth app."
