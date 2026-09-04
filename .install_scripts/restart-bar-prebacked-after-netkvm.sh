#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
droidvm=$app_root/bin/droidvm
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/bar-prebacked-18ac2713
staged_config=$stage/native-58065-bar8m-early-reboot.json
config=/data/local/tmp/droidvm-native-working-58065-8101a3b3.json
log=/data/local/tmp/crosvm-native-58065-8101a3b3.log
socket=$app_root/run/s-native-58065-bar8m-early-dc0dd9f6.sock
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
expected_name=s-native-58065-bar8m-early-reboot-8101a3b3

test -x "$app_crosvm"
test -x "$droidvm"
test -f "$staged_config"
test -f "$expected_image"
grep -F '"id":"8101a3b3-5395-49a9-9697-d4bd7de742fc"' "$staged_config" >/dev/null
grep -F '"memory_mb":2048' "$staged_config" >/dev/null
grep -F '"gpu_pci_bar_size":8388608' "$staged_config" >/dev/null
grep -F "$expected_image" "$staged_config" >/dev/null

pid=$(pidof crosvm 2>/dev/null || true)
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
case "$cmdline" in
    *"--name s-native-58065-bar8m-early-dc0dd9f6"*"--mem 2048"*"$expected_image"*) ;;
    *)
        echo "Refusing to power off unexpected crosvm: $cmdline" >&2
        exit 1
        ;;
esac

cp "$staged_config" "$config"
chmod 600 "$config"
sync
LD_LIBRARY_PATH="$library_path" "$app_crosvm" powerbtn "$socket"

attempt=0
while pidof crosvm >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 90 ]; then
        echo "Windows did not shut down within 90 seconds" >&2
        exit 1
    fi
    sleep 1
done

rm -f "$log"
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
            *"--name $expected_name"*"--mem 2048"*"$expected_image"*) break ;;
            *)
                echo "Unexpected crosvm after restart: $cmdline" >&2
                exit 1
                ;;
        esac
    fi
    attempt=$((attempt + 1))
    sleep 1
done
test -n "$pid"

stable=0
while [ "$stable" -lt 20 ]; do
    test "$(pidof crosvm 2>/dev/null || true)" = "$pid"
    stable=$((stable + 1))
    sleep 1
done

echo "shutdown_seconds=$attempt"
echo "crosvm_pid=$pid"
echo "config=$config"
echo "log=$log"
echo "$cmdline"
