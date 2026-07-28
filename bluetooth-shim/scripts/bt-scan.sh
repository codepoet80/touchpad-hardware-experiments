#!/usr/bin/env bash
# bt-scan.sh [seconds] -- raw BT inquiry via Luna, bypassing the settings app.
# Prints every discovered device with its Class-of-Device (cod) and name.
# Put the target device in pairing/discoverable mode first (e.g. Wii Remote:
# press 1+2; most mice: hold the pairing button until the LED blinks).
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
SECS="${1:-12}"
INQ=$(( SECS > 4 ? SECS - 2 : SECS ))
{
  echo "luna-send -i -n 40 luna://com.palm.bluetooth/gap/finddevices '{\"cod\":0,\"seconds\":$INQ,\"subscribe\":true}' 2>&1 &"
  echo 'P=$!'
  echo "sleep $SECS"
  echo 'kill $P 2>/dev/null'
  echo 'echo "--- scan done ---"'
} | $NOVACOM run file://bin/sh 2>&1 | grep -iE 'founddevice|address|"cod"|name|error' | grep -v BusyBox
