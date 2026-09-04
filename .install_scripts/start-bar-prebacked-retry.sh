#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
droidvm=$app_root/bin/droidvm
stage=/data/data/com.termux/files/home/bar-prebacked-a6ed148b
config=/data/local/tmp/droidvm-native-working-58063-6ce5aab3.json
staged_config=$stage/native-58063-bar8m-retry.json
log=/data/local/tmp/crosvm-native-58063-6ce5aab3.log
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
expected_name=s-native-58063-bar8m-retry-6ce5aab3

test -x "$droidvm"
test -f "$staged_config"
test -f "$expected_image"
test -z "$(pidof crosvm 2>/dev/null || true)"
grep -F '"id":"6ce5aab3-40ce-43bf-8f21-2923762c2398"' "$staged_config" >/dev/null
grep -F '"memory_mb":2048' "$staged_config" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$staged_config" >/dev/null
grep -F '"gpu_pci_bar_size":8388608' "$staged_config" >/dev/null
grep -F "$expected_image" "$staged_config" >/dev/null

cp "$staged_config" "$config"
chmod 600 "$config"
rm -f "$log"
sync
echo 3 >/proc/sys/vm/drop_caches 2>/dev/null || true
echo 1 >/proc/sys/vm/compact_memory 2>/dev/null || true
nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
echo "launcher_pid=$!"

attempt=0
pid=
while [ "$attempt" -lt 60 ]; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        test "$(echo "$pid" | wc -w)" -eq 1
        cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
        case "$cmdline" in
            *"--name $expected_name"*"--mem 2048"*"$expected_image"*) ;;
            *)
                echo "Unexpected crosvm: $cmdline" >&2
                exit 1
                ;;
        esac
        case "$cmdline" in
            *"--pre-alloc drm-host-mb=8"*"pci-bar-size=8388608"*) break ;;
            *)
                echo "Unexpected GPU memory configuration: $cmdline" >&2
                exit 1
                ;;
        esac
    fi
    attempt=$((attempt + 1))
    sleep 1
done
test -n "$pid"

stable=0
while [ "$stable" -lt 15 ]; do
    current=$(pidof crosvm 2>/dev/null || true)
    test "$current" = "$pid"
    stable=$((stable + 1))
    sleep 1
done

echo "crosvm_pid=$pid"
echo "config=$config"
echo "log=$log"
echo "$cmdline"
