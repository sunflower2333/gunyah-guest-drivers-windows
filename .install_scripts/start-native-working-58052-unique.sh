#!/system/bin/sh
set -eu

droidvm=/data/data/cn.classfun.droidvm/bin/droidvm
source=/data/local/tmp/droidvm-native-working-58052-retry2.json
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
old_id=39a47d25-92f3-42ee-ae03-627c0c00fb46
old_name=s-native-58052-simplefb-retry2

test -x "$droidvm"
test -f "$source"
test -f "$expected_image"
test -z "$(pidof crosvm 2>/dev/null || true)"
grep -F '"memory_mb":2048' "$source" >/dev/null
grep -F '"gpu_udmabuf":true' "$source" >/dev/null
grep -F '"gpu_mode":"native"' "$source" >/dev/null
grep -F '"gpu_provider":"drm2kgsl"' "$source" >/dev/null
grep -F '"display_backend":"simplefb"' "$source" >/dev/null
grep -F "$expected_image" "$source" >/dev/null

new_id="$(cat /proc/sys/kernel/random/uuid)"
short_id="$(echo "$new_id" | cut -c1-8)"
new_name="s-native-58052-r$short_id"
config="/data/local/tmp/droidvm-native-working-58052-$short_id.json"

sed -e "s|\"id\":\"$old_id\"|\"id\":\"$new_id\"|g" \
    -e "s|\"name\":\"$old_name\"|\"name\":\"$new_name\"|g" \
    "$source" >"$config"
grep -F "\"id\":\"$new_id\"" "$config" >/dev/null
grep -F "\"name\":\"$new_name\"" "$config" >/dev/null
grep -F "$expected_image" "$config" >/dev/null

echo "config=$config"
echo "vm_id=$new_id"
echo "vm_name=$new_name"
exec "$droidvm" start "$(cat "$config")"
