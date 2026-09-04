#!/system/bin/sh
set -eu

source=/data/local/tmp/droidvm-native-inner-0254b874.json
expected=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
droidvm=/data/data/cn.classfun.droidvm/bin/droidvm

test -f "$source"
test -f "$expected"
test -z "$(pidof crosvm 2>/dev/null || true)"
grep -F '"memory_mb":2048' "$source" >/dev/null
grep -F '"gpu_udmabuf":true' "$source" >/dev/null
grep -F '"gpu_mode":"native"' "$source" >/dev/null
grep -F '"gpu_provider":"drm2kgsl"' "$source" >/dev/null
grep -F "$expected" "$source" >/dev/null

id="$(cat /proc/sys/kernel/random/uuid)"
short="$(echo "$id" | cut -c1-8)"
name="s-native-58068-inner-r$short"
tap="vmd$short-0"
config="/data/local/tmp/droidvm-native-inner-$short.json"

sed -e "s/0254b874-6d1a-4478-9325-386c6ef7ca36/$id/g" \
    -e "s/s-native-58068-inner-0254b874/$name/g" \
    -e "s/vmd1ea2c7c-0/$tap/g" \
    "$source" > "$config"

grep -F "\"id\":\"$id\"" "$config" >/dev/null
grep -F "\"name\":\"$name\"" "$config" >/dev/null
grep -F "\"tap_name\":\"$tap\"" "$config" >/dev/null
grep -F "$expected" "$config" >/dev/null

echo "config=$config"
echo "vm_id=$id"
echo "vm_name=$name"
exec "$droidvm" start "$(cat "$config")"
