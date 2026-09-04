#!/system/bin/sh
set -eu

pid="$(pidof crosvm 2>/dev/null || true)"
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline="$(tr '\000' ' ' < "/proc/$pid/cmdline")"
case "$cmdline" in
    *"--mem 2048"*"win11-droidvm-final-comp.native-working-58052.qcow2"*) ;;
    *)
        echo "Refusing to stop unexpected crosvm: $cmdline" >&2
        exit 1
        ;;
esac

socket=/data/data/cn.classfun.droidvm/run/s-native-58052-r369497a0.sock
crosvm=/data/data/cn.classfun.droidvm/usr/bin/crosvm
LD_LIBRARY_PATH=/data/data/cn.classfun.droidvm/usr/lib:/data/data/cn.classfun.droidvm/lib \
    "$crosvm" stop "$socket"

attempt=0
while pidof crosvm >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 60
    sleep 1
done

exec sh /data/data/com.termux/files/home/start-bar-prebacked-retry.sh
