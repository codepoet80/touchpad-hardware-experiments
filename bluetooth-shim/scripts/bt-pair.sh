#!/usr/bin/env bash
# bt-pair.sh <address> [cod] -- pair a device by BD_ADDR via Luna, and watch the
# pairing + profile-connect notifications for ~20s.  Bypasses the settings app.
# Example: scripts/bt-pair.sh 00:1E:35:AA:BB:CC 0x002504
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
ADDR="${1:?usage: bt-pair.sh <address> [cod]}"
COD="${2:-0}"
{
  # watch GAP pairing + profile notifications in the background
  echo "luna-send -i -n 60 luna://com.palm.bluetooth/gap/subscribepair '{\"subscribe\":true}' 2>&1 & P1=\$!"
  echo "luna-send -i -n 60 luna://com.palm.bluetooth/prof/subscribenotifications '{\"subscribe\":true}' 2>&1 & P2=\$!"
  echo 'sleep 1'
  echo "luna-send -n 1 luna://com.palm.bluetooth/gap/pair '{\"address\":\"$ADDR\",\"cod\":$COD}' 2>&1"
  echo 'sleep 20'
  echo 'kill $P1 $P2 2>/dev/null'
  echo 'echo "--- pair watch done ---"'
} | $NOVACOM run file://bin/sh 2>&1 | grep -v BusyBox | grep -iE 'pair|passkey|pin|connect|error|address|returnValue|profile|ssp' | head -40
