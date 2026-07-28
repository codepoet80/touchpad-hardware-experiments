#!/bin/sh
# One pairing pass. NEVER loop profconnect: only 7 HIDH sub-instances exist and
# each failed connect consumes one permanently (result 0xB, supplier 26).
ADDR="84:17:66:d5:ff:51"
lc(){ (luna-send -i "$1" "$2" 2>&1 & P=$!; sleep ${3:-2}; kill $P 2>/dev/null); }
lc palm://com.palm.bluetooth/gap/finddevices '{"cod":0,"seconds":12,"subscribe":true}' 15 >/dev/null
luna-send -i palm://com.palm.bluetooth/gap/subscribepair '{"subscribe":true}' > /tmp/wz.sub 2>&1 &
SUB=$!
sleep 1
lc palm://com.palm.bluetooth/gap/pair "{\"address\":\"$ADDR\",\"cod\":9480}" 3 >/dev/null
i=0; ACK=0
while [ $i -lt 30 ]; do
  if [ $ACK -eq 0 ] && grep -q sspjustworks /tmp/wz.sub; then
    lc palm://com.palm.bluetooth/gap/ssppairaccept "{\"accept\":true,\"address\":\"$ADDR\"}" 2 >/dev/null
    ACK=1
  fi
  grep -q '"notification":"notifnpaired".*"error":0' /tmp/wz.sub && break
  sleep 1; i=$((i+1))
done
kill $SUB 2>/dev/null
lc palm://com.palm.bluetooth/prof/profconnect "{\"profile\":\"hid\",\"address\":\"$ADDR\"}" 4 >/dev/null
