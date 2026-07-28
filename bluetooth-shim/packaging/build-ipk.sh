#!/usr/bin/env bash
# build-ipk.sh -- assemble org.webosarchive.btgamepad_<ver>_armv7.ipk
# Payload: the built shim + udev rule. Control: control.in (version/date filled),
# postinst, prerm. Final ipk is written to the project-root ipks/ folder.
set -euo pipefail
cd "$(dirname "$0")"

PID=org.webosarchive.btgamepad
VERSION="${1:-1.1.0}"
ARCH=armv7
SHIM=../libpmbtgamepad.so
STAMP="$(date +%s)"
OUT="${PID}_${VERSION}_${ARCH}.ipk"
OUTDIR="../../ipks"          # project-root ipks/ folder

# 1. ensure the shim is built
if [ ! -f "$SHIM" ]; then
    echo ">> shim not built; running make in $(cd .. && pwd)"
    ( cd .. && make )
fi
file "$SHIM" | grep -q ARM || { echo "ERROR: $SHIM is not an ARM binary"; exit 1; }

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
mkdir -p "$BUILD/control" "$BUILD/data/usr/palm/applications/$PID/files"

# 2. payload
cp "$SHIM" "$BUILD/data/usr/palm/applications/$PID/files/libpmbtgamepad.so"
cp 99-bt-gamepad.rules "$BUILD/data/usr/palm/applications/$PID/files/99-bt-gamepad.rules"

# 3. control (fill version + timestamp), maintainer scripts
sed -e "s/@VERSION@/$VERSION/" -e "s/@LASTUPDATED@/$STAMP/" control/control.in > "$BUILD/control/control"
install -m 0755 control/postinst "$BUILD/control/postinst"
install -m 0755 control/prerm    "$BUILD/control/prerm"

# 4. assemble ipk (debian-binary + control.tar.gz + data.tar.gz, this member order)
printf '2.0\n' > "$BUILD/debian-binary"
( cd "$BUILD/control" && tar --owner=0 --group=0 -czf ../control.tar.gz . )
( cd "$BUILD/data"    && tar --owner=0 --group=0 -czf ../data.tar.gz . )
( cd "$BUILD" && ar rc "$OUT" debian-binary control.tar.gz data.tar.gz )
mkdir -p "$OUTDIR"
cp "$BUILD/$OUT" "$OUTDIR/$OUT"

echo ">> built $OUTDIR/$OUT"
ls -la "$OUTDIR/$OUT"
echo ">> md5: $(md5sum "$OUTDIR/$OUT" | cut -d' ' -f1)  size: $(stat -c%s "$OUTDIR/$OUT")"
