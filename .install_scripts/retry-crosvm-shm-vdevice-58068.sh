#!/system/bin/sh
set -eu

droidvm=/data/data/cn.classfun.droidvm/bin/droidvm
app_crosvm=/data/data/cn.classfun.droidvm/usr/bin/crosvm
config=/data/local/tmp/droidvm-native-working-58068-c1c1f4f4.json
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
log=/data/local/tmp/crosvm-native-58068-c1c1f4f4-retry.log
console_stderr=/data/data/cn.classfun.droidvm/cache/console_c1c1f4f4-1e81-4c40-bcf6-6217c6da69e0_stderr.log
console_stdio=/data/data/cn.classfun.droidvm/cache/console_c1c1f4f4-1e81-4c40-bcf6-6217c6da69e0_stdio.log
expected_hash=27c7d486e985b3c16cd08c1c201bd0d36d47b8052c9eea7611f258e9754d4636

test -z "$(pidof crosvm 2>/dev/null || true)"
test -x "$droidvm"
test -x "$app_crosvm"
test -f "$config"
test -f "$image"
test "$(sha256sum "$app_crosvm" | awk '{print $1}')" = "$expected_hash"
grep -F '"id":"c1c1f4f4-1e81-4c40-bcf6-6217c6da69e0"' "$config" >/dev/null
grep -F '"name":"s-native-58068-shm-c1c1f4f4"' "$config" >/dev/null
grep -F '"memory_mb":2048' "$config" >/dev/null
grep -F "$image" "$config" >/dev/null

rm -f "$console_stderr" "$console_stdio"
: >"$log"
nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
echo "launcher_pid=$!"
