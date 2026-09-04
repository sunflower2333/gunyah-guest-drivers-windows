#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
crosvm=$app_root/usr/bin/crosvm
renderer=$app_root/usr/lib/libvirglrenderer.so
python=/data/data/com.termux/files/usr/bin/python3
start_existing=/data/data/com.termux/files/home/exclusive-bar-d9645060/droidvm-start-existing.py
config=/data/local/tmp/droidvm-native-working-58068-retry-458ffb2c.json
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
log=/data/local/tmp/crosvm-native-58068-old-baseline-458ffb2c.log
expected_id=458ffb2c-a1e9-4830-bec5-4e1fe323b493
expected_name=s-native-58068-shm-retry-458ffb2c
expected_crosvm_hash=27c7d486e985b3c16cd08c1c201bd0d36d47b8052c9eea7611f258e9754d4636
expected_renderer_hash=50bf1dc1a5e75ea45a56a0b13c1c7e7b10a205856c0de8dda19140471ef946f6

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

printf 'preflight_crosvm_pid=%s\n' "$(pidof crosvm 2>/dev/null || true)"
test -z "$(pidof crosvm 2>/dev/null || true)"
test -x "$crosvm"
test -x "$python"
test -f "$renderer"
test -f "$start_existing"
test -f "$config"
test -f "$image"
test "$(hash_file "$crosvm")" = "$expected_crosvm_hash"
test "$(hash_file "$renderer")" = "$expected_renderer_hash"
grep -F "\"id\":\"$expected_id\"" "$config" >/dev/null
grep -F "\"name\":\"$expected_name\"" "$config" >/dev/null
grep -F '"memory_mb":2048' "$config" >/dev/null
grep -F '"gpu_udmabuf":true' "$config" >/dev/null
grep -F "$image" "$config" >/dev/null

rm -f "$log"
if ! "$python" "$start_existing" "$expected_id" >"$log" 2>&1; then
    cat "$log"
    exit 1
fi

attempt=0
pid=
while [ "$attempt" -lt 90 ]; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        test "$(echo "$pid" | wc -w)" -eq 1
        cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
        case "$cmdline" in
            *"--name $expected_name"*"--mem 2048"*"$image"*) break ;;
            *) echo "unexpected crosvm: $cmdline" >&2; exit 1 ;;
        esac
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ -z "$pid" ]; then
    cat "$log"
    exit 1
fi

stable=0
while [ "$stable" -lt 30 ]; do
    if [ "$(pidof crosvm 2>/dev/null || true)" != "$pid" ]; then
        cat "$log"
        exit 1
    fi
    stable=$((stable + 1))
    sleep 1
done

echo "crosvm_pid=$pid"
echo "$cmdline"
echo "log=$log"
