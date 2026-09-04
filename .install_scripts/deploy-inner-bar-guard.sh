#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_renderer=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
config_template=/data/local/tmp/droidvm-native-working-58068-retry-458ffb2c.json
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
stage=/data/data/com.termux/files/home/bar-inner-guard
backup_dir=/data/local/tmp/droidvm-sda17/back/host
new_crosvm_hash=b3c1a01d0a7d9b7eb7d3e3d69c7327fa0f2a9a29ba9f3e6286f7acd38384d6ae
new_renderer_hash=8f185f4c31fb2c6326e32e0be13fd9ba68a487505ef1b62c19d086701fe44d50

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

make_config()
{
    id=$(cat /proc/sys/kernel/random/uuid)
    short=$(echo "$id" | cut -c1-8)
    name=s-native-58068-inner-$short
    config=/data/local/tmp/droidvm-native-inner-$short.json
    socket=$app_root/run/$name.sock
    log=/data/local/tmp/crosvm-native-58068-inner-bar-guard-$short.log
    sed \
        -e "s/458ffb2c-a1e9-4830-bec5-4e1fe323b493/$id/g" \
        -e "s/s-native-58068-shm-retry-458ffb2c/$name/g" \
        "$config_template" >"$config"
}

install_file()
{
    source_file=$1
    target=$2
    expected_hash=$3
    owner=$(stat -c '%u:%g' "$target")
    mode=$(stat -c '%a' "$target")
    context=$(ls -Zd "$target" | awk '{print $1}')
    temporary=$target.deploy-new
    rm -f "$temporary"
    cp "$source_file" "$temporary"
    chown "$owner" "$temporary"
    chmod "$mode" "$temporary"
    chcon "$context" "$temporary"
    test "$(hash_file "$temporary")" = "$expected_hash"
    mv -f "$temporary" "$target"
    sync
    test "$(hash_file "$target")" = "$expected_hash"
}

test -f "$stage/crosvm"
test -f "$stage/libvirglrenderer.so"
test -f "$config_template"
test -f "$image"
test "$(hash_file "$stage/crosvm")" = "$new_crosvm_hash"
test "$(hash_file "$stage/libvirglrenderer.so")" = "$new_renderer_hash"
grep -F '"memory_mb":2048' "$config_template" >/dev/null
grep -F "$image" "$config_template" >/dev/null

old_crosvm_hash=$(hash_file "$app_crosvm")
old_renderer_hash=$(hash_file "$app_renderer")
pid=$(pidof crosvm 2>/dev/null || true)
test "$(echo "$pid" | wc -w)" -le 1
mkdir -p "$backup_dir"
cp -p "$app_crosvm" "$backup_dir/crosvm-$old_crosvm_hash"
cp -p "$app_renderer" "$backup_dir/libvirglrenderer-$old_renderer_hash.so"
sync

if [ -n "$pid" ]; then
    LD_LIBRARY_PATH="$app_root/usr/lib" "$app_crosvm" stop "$app_root/run/s-native-58068-shm-retry-458ffb2c.sock"
fi
attempt=0
while [ -n "$(pidof crosvm 2>/dev/null || true)" ] && [ "$attempt" -lt 45 ]; do
    sleep 1
    attempt=$((attempt + 1))
done
test -z "$(pidof crosvm 2>/dev/null || true)"

install_file "$stage/crosvm" "$app_crosvm" "$new_crosvm_hash"
install_file "$stage/libvirglrenderer.so" "$app_renderer" "$new_renderer_hash"
make_config
rm -f "$log"
nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &

attempt=0
pid=
while [ "$attempt" -lt 60 ]; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        test "$(echo "$pid" | wc -w)" -eq 1
        break
    fi
    sleep 1
    attempt=$((attempt + 1))
done

stable=0
while [ -n "$pid" ] && [ "$stable" -lt 20 ]; do
    current=$(pidof crosvm 2>/dev/null || true)
    if [ "$current" != "$pid" ]; then
        pid=
        break
    fi
    stable=$((stable + 1))
    sleep 1
done

if [ -z "$pid" ] || [ "$stable" -ne 20 ]; then
    test -z "$(pidof crosvm 2>/dev/null || true)"
    install_file "$backup_dir/crosvm-$old_crosvm_hash" "$app_crosvm" "$old_crosvm_hash"
    install_file "$backup_dir/libvirglrenderer-$old_renderer_hash.so" "$app_renderer" "$old_renderer_hash"
    make_config
    nohup "$droidvm" start "$(cat "$config")" >"$log.rollback" 2>&1 </dev/null &
    echo "inner BAR guard launch failed; previous binaries restored" >&2
    exit 1
fi

echo "crosvm_pid=$pid"
echo "crosvm_hash=$(hash_file "$app_crosvm")"
echo "renderer_hash=$(hash_file "$app_renderer")"
echo "config=$config"
echo "log=$log"
