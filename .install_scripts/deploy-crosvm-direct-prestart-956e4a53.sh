#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
droidvm=$app_root/bin/droidvm
stage=/data/data/com.termux/files/home/crosvm-direct-prestart-956e4a53
source_config=/data/data/com.termux/files/home/bar-prebacked-18ac2713/native-58065-bar8m-early-reboot.json
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
backup_dir=/data/local/tmp/droidvm-sda17/back/host
old_hash=18ac271394d868304ad37c4b05483865cea96c4618c091a68f716bbb2c3bb794
new_hash=956e4a538ffd773cf31697d5cb723bde70d5a78256298c455a2234f155193ecd

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

test -x "$app_crosvm"
test -x "$droidvm"
test -f "$stage/crosvm"
test -f "$source_config"
test -f "$expected_image"
test "$(hash_file "$app_crosvm")" = "$old_hash"
test "$(hash_file "$stage/crosvm")" = "$new_hash"
grep -F '"memory_mb":2048' "$source_config" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$source_config" >/dev/null
grep -F '"gpu_pci_bar_size":8388608' "$source_config" >/dev/null
grep -F "$expected_image" "$source_config" >/dev/null

pid=$(pidof crosvm 2>/dev/null || true)
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
case "$cmdline" in
    *"--name s-native-58065-bar8m-early-reboot-8101a3b3"*"--mem 2048"*"$expected_image"*) ;;
    *)
        echo "Refusing to stop unexpected crosvm: $cmdline" >&2
        exit 1
        ;;
esac
test "$(find /proc/$pid/task -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 1

owner=$(stat -c '%u:%g' "$app_crosvm")
mode=$(stat -c '%a' "$app_crosvm")
context=$(ls -Zd "$app_crosvm" | awk '{print $1}')
mkdir -p "$backup_dir"
backup=$backup_dir/crosvm-$old_hash
if [ -f "$backup" ]; then
    test "$(hash_file "$backup")" = "$old_hash"
else
    cp -p "$app_crosvm" "$backup"
    sync
    test "$(hash_file "$backup")" = "$old_hash"
fi

kill "$pid"
attempt=0
while kill -0 "$pid" 2>/dev/null && [ "$attempt" -lt 10 ]; do
    attempt=$((attempt + 1))
    sleep 1
done
if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid"
    sleep 1
fi
test -z "$(pidof crosvm 2>/dev/null || true)"

temporary=$app_crosvm.deploy-new
rm -f "$temporary"
cp "$stage/crosvm" "$temporary"
chown "$owner" "$temporary"
chmod "$mode" "$temporary"
chcon "$context" "$temporary"
test "$(hash_file "$temporary")" = "$new_hash"
mv -f "$temporary" "$app_crosvm"
sync
test "$(hash_file "$app_crosvm")" = "$new_hash"

new_id=$(cat /proc/sys/kernel/random/uuid)
short_id=$(echo "$new_id" | cut -c1-8)
new_name=s-native-58066-direct-$short_id
new_config=/data/local/tmp/droidvm-native-working-58066-$short_id.json
new_log=/data/local/tmp/crosvm-native-58066-$short_id.log
sed \
    -e "s|\"id\":\"8101a3b3-5395-49a9-9697-d4bd7de742fc\"|\"id\":\"$new_id\"|g" \
    -e "s|\"name\":\"s-native-58065-bar8m-early-reboot-8101a3b3\"|\"name\":\"$new_name\"|g" \
    "$source_config" >"$new_config"
grep -F "\"id\":\"$new_id\"" "$new_config" >/dev/null
grep -F "\"name\":\"$new_name\"" "$new_config" >/dev/null
grep -F "$expected_image" "$new_config" >/dev/null
: >"$new_log"
nohup "$droidvm" start "$(cat "$new_config")" >"$new_log" 2>&1 </dev/null &

echo "launcher_pid=$!"
echo "vm_id=$new_id"
echo "vm_name=$new_name"
echo "config=$new_config"
echo "log=$new_log"
echo "crosvm_hash=$(hash_file "$app_crosvm")"
