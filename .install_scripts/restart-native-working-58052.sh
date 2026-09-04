#!/system/bin/sh
set -eu

droidvm=/data/data/cn.classfun.droidvm/bin/droidvm
crosvm=/data/data/cn.classfun.droidvm/usr/bin/crosvm
config=/data/local/tmp/droidvm-native-working-58052-retry2.json
socket=/data/data/cn.classfun.droidvm/run/s-native-58052-simplefb-retry2.sock
log=/data/local/tmp/crosvm-native-58052.log
library_path=/data/data/cn.classfun.droidvm/usr/lib:/data/data/cn.classfun.droidvm/lib
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2

test -x "$droidvm"
test -x "$crosvm"
test -f "$config"
test -f "$expected_image"
grep -F '"memory_mb":2048' "$config" >/dev/null
grep -F '"gpu_udmabuf":true' "$config" >/dev/null
grep -F '"gpu_mode":"native"' "$config" >/dev/null
grep -F '"gpu_provider":"drm2kgsl"' "$config" >/dev/null
grep -F '"display_backend":"simplefb"' "$config" >/dev/null
grep -F "$expected_image" "$config" >/dev/null

pid="$(pidof crosvm 2>/dev/null || true)"
test -n "$pid"
cmdline="$(tr '\000' ' ' < "/proc/$pid/cmdline")"
case "$cmdline" in
    *"--mem 2048"*"$expected_image"*) ;;
    *)
        echo "Refusing to stop unexpected crosvm: $cmdline" >&2
        exit 1
        ;;
esac

LD_LIBRARY_PATH="$library_path" "$crosvm" stop "$socket"
attempt=0
while pidof crosvm >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 30 ]; then
        echo "crosvm did not stop within 30 seconds" >&2
        exit 1
    fi
    sleep 1
done

if [ -f "$log" ]; then
    mv "$log" "$log.pre-network-restart"
fi
nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
echo "launcher_pid=$!"

attempt=0
while :; do
    pid="$(pidof crosvm 2>/dev/null || true)"
    if [ -n "$pid" ]; then
        cmdline="$(tr '\000' ' ' < "/proc/$pid/cmdline")"
        case "$cmdline" in
            *"--mem 2048"*"$expected_image"*)
                echo "crosvm_pid=$pid"
                echo "$cmdline"
                exit 0
                ;;
            *)
                echo "Unexpected crosvm after restart: $cmdline" >&2
                exit 1
                ;;
        esac
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 30 ]; then
        echo "crosvm did not start within 30 seconds" >&2
        exit 1
    fi
    sleep 1
done
