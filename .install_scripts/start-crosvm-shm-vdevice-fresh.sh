#!/system/bin/sh
set -eu

droidvm=/data/data/cn.classfun.droidvm/bin/droidvm
app_crosvm=/data/data/cn.classfun.droidvm/usr/bin/crosvm
source_config=/data/local/tmp/droidvm-native-working-58067-5f067549.json
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
expected_hash=27c7d486e985b3c16cd08c1c201bd0d36d47b8052c9eea7611f258e9754d4636

test -z "$(pidof crosvm 2>/dev/null || true)"
test -x "$droidvm"
test -x "$app_crosvm"
test -f "$source_config"
test -f "$image"
test "$(sha256sum "$app_crosvm" | awk '{print $1}')" = "$expected_hash"
grep -F '"id":"5f067549-1fa7-488c-8398-fee9ece79c1d"' "$source_config" >/dev/null
grep -F '"name":"s-native-58067-direct-5f067549"' "$source_config" >/dev/null
grep -F '"memory_mb":2048' "$source_config" >/dev/null
grep -F "$image" "$source_config" >/dev/null

new_id=$(cat /proc/sys/kernel/random/uuid)
short_id=$(echo "$new_id" | cut -c1-8)
new_name=s-native-58068-shm-retry-$short_id
new_config=/data/local/tmp/droidvm-native-working-58068-retry-$short_id.json
new_log=/data/local/tmp/crosvm-native-58068-retry-$short_id.log
sed \
    -e "s|\"id\":\"5f067549-1fa7-488c-8398-fee9ece79c1d\"|\"id\":\"$new_id\"|g" \
    -e "s|\"name\":\"s-native-58067-direct-5f067549\"|\"name\":\"$new_name\"|g" \
    "$source_config" >"$new_config"
grep -F "\"id\":\"$new_id\"" "$new_config" >/dev/null
grep -F "\"name\":\"$new_name\"" "$new_config" >/dev/null
grep -F '"memory_mb":2048' "$new_config" >/dev/null
grep -F "$image" "$new_config" >/dev/null
: >"$new_log"
nohup "$droidvm" start "$(cat "$new_config")" >"$new_log" 2>&1 </dev/null &

echo "launcher_pid=$!"
echo "vm_id=$new_id"
echo "vm_name=$new_name"
echo "config=$new_config"
echo "log=$new_log"
