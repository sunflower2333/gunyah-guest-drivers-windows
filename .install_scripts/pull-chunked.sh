#!/bin/bash
set -u
SRC="$1"; DST="$2"; DEV="192.168.60.237:5555"; BS=65536
SIZE=$(adb -s $DEV shell "stat -c%s $SRC" 2>/dev/null | tr -d '\r\n')
N=$(( (SIZE + BS - 1) / BS ))
echo "size=$SIZE chunks=$N"
: > "$DST"
for ((i=0; i<N; i++)); do
    ok=0
    for try in 1 2 3 4; do
        adb -s $DEV exec-out "dd if=$SRC bs=$BS skip=$i count=1 2>/dev/null" > "$DST.part" 2>/dev/null
        got=$(stat -c%s "$DST.part")
        want=$BS; last=$(( SIZE - i*BS )); [ $last -lt $BS ] && want=$last
        if [ "$got" = "$want" ]; then ok=1; break; fi
        adb disconnect $DEV >/dev/null 2>&1; adb connect $DEV >/dev/null 2>&1
    done
    [ $ok -ne 1 ] && { echo "FAILED chunk $i"; exit 1; }
    cat "$DST.part" >> "$DST"; printf '.'
done
rm -f "$DST.part"; echo; stat -c%s "$DST"
