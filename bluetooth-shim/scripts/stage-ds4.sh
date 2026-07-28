#!/usr/bin/env bash
#
# stage-ds4.sh -- post-boot staging for a DS4 test session.
# Verifies the shim is loaded, the stock library baseline is in place, turns the
# radio on (verified, with retry -- radioon can silently fail), and reports
# what's ready.  Safe to re-run.
#
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
dev_sh() { $NOVACOM run file://bin/sh; }

{
  echo 'echo "=== shim ==="'
  echo 'PE=$(pidof PmBtEngine || true)'
  echo 'if [ -n "$PE" ] && grep -q libpmbtgamepad /proc/$PE/maps; then echo "OK: shim mapped in PmBtEngine ($PE)"; else echo "WARN: shim NOT in PmBtEngine maps (PE=$PE)"; fi'
  echo 'grep -m1 "GOT-patching" /var/log/btshim.log 2>/dev/null || echo "WARN: no GOT-patching line in btshim.log"'

  echo 'echo "=== library baseline ==="'
  echo 'if [ -f /usr/lib/libPmBtBsaif.so.orig ]; then
          a=$(md5sum /usr/lib/libPmBtBsaif.so | cut -d" " -f1)
          b=$(md5sum /usr/lib/libPmBtBsaif.so.orig | cut -d" " -f1)
          if [ "$a" = "$b" ]; then echo "OK: live lib == stock .orig"; else echo "WARN: live libPmBtBsaif.so DIFFERS from .orig (keycode-table patch still installed?)"; fi
        else echo "note: no .orig on device (nothing to compare)"; fi'

  echo 'echo "=== sysrq ==="; cat /proc/sys/kernel/sysrq'

  echo 'echo "=== radio ==="'
  echo 'for try in 1 2 3; do
          if pidof PmBtStack >/dev/null; then echo "OK: PmBtStack running (radio on)"; break; fi
          echo "radioon attempt $try..."
          luna-send -n 1 palm://com.palm.btmonitor/monitor/radioon "{\"visible\":true,\"connectable\":true}" 2>&1
          sleep 6
        done'
  echo 'pidof PmBtStack >/dev/null || echo "WARN: radio still off after 3 attempts"'
  echo 'echo "=== done ==="'
} | dev_sh
