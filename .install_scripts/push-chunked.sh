#!/bin/bash
# The wireless ADB link truncates sustained transfers, so push in verified
# chunks and reassemble on the device.
set -u
SRC="$1"; DST="$2"; DEV="192.168.60.237:5555"; CHUNK=524288
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
split -b $CHUNK -d -a 3 "$SRC" "$TMP/part."
N=$(ls "$TMP" | wc -l)
echo "chunks=$N"
adb -s $DEV shell "rm -rf /data/local/tmp/chunks; mkdir -p /data/local/tmp/chunks" >/dev/null 2>&1
for f in "$TMP"/part.*; do
    b=$(basename "$f"); want=$(stat -c%s "$f"); ok=0
    for try in 1 2 3 4; do
        adb -s $DEV push "$f" "/data/local/tmp/chunks/$b" >/dev/null 2>&1
        got=$(adb -s $DEV shell "stat -c%s /data/local/tmp/chunks/$b 2>/dev/null" 2>/dev/null | tr -d '\r\n')
        if [ "$got" = "$want" ]; then ok=1; break; fi
        adb disconnect $DEV >/dev/null 2>&1; adb connect $DEV >/dev/null 2>&1
    done
    if [ $ok -ne 1 ]; then echo "FAILED $b (want=$want got=$got)"; exit 1; fi
    printf '.'
done
echo
adb -s $DEV shell "cat /data/local/tmp/chunks/part.* > $DST; sha256sum $DST; stat -c%s $DST" 2>&1 | tr -d '\r'
