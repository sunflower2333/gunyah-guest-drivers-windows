#!/system/bin/sh
set -eu

id=0254b874-6d1a-4478-9325-386c6ef7ca36
config=/data/local/tmp/droidvm-native-inner-0254b874.json
expected=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
droidvm=/data/data/cn.classfun.droidvm/bin/droidvm

pid="$(pidof crosvm 2>/dev/null || true)"
test -n "$pid"
cmdline="$(tr '\000' ' ' < "/proc/$pid/cmdline")"
case "$cmdline" in
    *"--mem 2048"*"$expected"*) ;;
    *) echo "unexpected crosvm: $cmdline" >&2; exit 2 ;;
esac

"$droidvm" stop "$id"
for n in $(seq 1 30); do
    if ! pidof crosvm >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
test -z "$(pidof crosvm 2>/dev/null || true)"

"$droidvm" start "$(cat "$config")"
