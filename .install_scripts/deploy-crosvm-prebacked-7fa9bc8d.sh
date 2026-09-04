#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_renderer=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
stage=/data/data/com.termux/files/home/bar-prebacked-7fa9bc8d
backup_dir=/data/local/tmp/droidvm-sda17/back/host
new_config=/data/local/tmp/droidvm-native-working-58064-7e795227.json
rollback_config=/data/local/tmp/droidvm-native-working-58062-rollback-65967948.json
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
new_name=s-native-58064-bar8m-fixed-7e795227
new_log=/data/local/tmp/crosvm-native-58064-7e795227.log
rollback_log=/data/local/tmp/crosvm-native-58062-rollback-65967948.log

old_hash=a6ed148b5ce71b0b2dec866edc4adcc997438985ec66a6535b96a1c1094dda48
new_hash=7fa9bc8dadf2b826336af9b6b4970e25e7a08119acaf40a39f282f24abf87c7b
renderer_hash=50bf1dc1a5e75ea45a56a0b13c1c7e7b10a205856c0de8dda19140471ef946f6

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

install_crosvm()
{
    source_file=$1
    expected_hash=$2
    temporary=$app_crosvm.deploy-new
    owner=$(stat -c '%u:%g' "$app_crosvm")
    mode=$(stat -c '%a' "$app_crosvm")
    context=$(ls -Zd "$app_crosvm" | awk '{print $1}')

    rm -f "$temporary"
    cp "$source_file" "$temporary"
    chown "$owner" "$temporary"
    chmod "$mode" "$temporary"
    chcon "$context" "$temporary"
    test "$(hash_file "$temporary")" = "$expected_hash"
    mv -f "$temporary" "$app_crosvm"
    sync
    test "$(hash_file "$app_crosvm")" = "$expected_hash"
}

start_config()
{
    config=$1
    log=$2
    rm -f "$log"
    nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
    echo "launcher_pid=$!"
}

wait_for_vm()
{
    expected_name=$1
    expected_bar=$2
    expected_pid=
    attempt=0
    while [ "$attempt" -lt 60 ]; do
        current=$(pidof crosvm 2>/dev/null || true)
        if [ -n "$current" ]; then
            [ "$(echo "$current" | wc -w)" -eq 1 ] || return 2
            cmdline=$(tr '\000' ' ' <"/proc/$current/cmdline")
            case "$cmdline" in
                *"--name $expected_name"*"--mem 2048"*"$expected_image"*) ;;
                *) return 2 ;;
            esac
            case "$cmdline" in
                *"--pre-alloc drm-host-mb=8"*"pci-bar-size=$expected_bar"*)
                    expected_pid=$current
                    break
                    ;;
                *) return 2 ;;
            esac
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    [ -n "$expected_pid" ] || return 1

    stable=0
    while [ "$stable" -lt 20 ]; do
        current=$(pidof crosvm 2>/dev/null || true)
        [ "$current" = "$expected_pid" ] || return 1
        stable=$((stable + 1))
        sleep 1
    done
    echo "crosvm_pid=$expected_pid"
    echo "$cmdline"
    return 0
}

test -z "$(pidof crosvm 2>/dev/null || true)"
test -x "$app_crosvm"
test -x "$droidvm"
test -f "$app_renderer"
test -f "$stage/crosvm"
test -f "$stage/native-58064-bar8m-fixed.json"
test -f "$stage/native-58062-rollback.json"
test -f "$expected_image"
test "$(hash_file "$app_crosvm")" = "$old_hash"
test "$(hash_file "$app_renderer")" = "$renderer_hash"
test "$(hash_file "$stage/crosvm")" = "$new_hash"
grep -F '"id":"7e795227-080d-4f7e-8b2d-248d5966090f"' "$stage/native-58064-bar8m-fixed.json" >/dev/null
grep -F '"memory_mb":2048' "$stage/native-58064-bar8m-fixed.json" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$stage/native-58064-bar8m-fixed.json" >/dev/null
grep -F '"gpu_pci_bar_size":8388608' "$stage/native-58064-bar8m-fixed.json" >/dev/null
grep -F "$expected_image" "$stage/native-58064-bar8m-fixed.json" >/dev/null

mkdir -p "$backup_dir"
backup=$backup_dir/crosvm-$old_hash
if [ -f "$backup" ]; then
    test "$(hash_file "$backup")" = "$old_hash"
else
    cp -p "$app_crosvm" "$backup"
    sync
    test "$(hash_file "$backup")" = "$old_hash"
fi
cp "$stage/native-58064-bar8m-fixed.json" "$new_config"
cp "$stage/native-58062-rollback.json" "$rollback_config"
chmod 600 "$new_config" "$rollback_config"

install_crosvm "$stage/crosvm" "$new_hash"
start_config "$new_config" "$new_log"
if wait_for_vm "$new_name" 8388608; then
    echo "crosvm_hash=$(hash_file "$app_crosvm")"
    echo "renderer_hash=$(hash_file "$app_renderer")"
    echo "config=$new_config"
    echo "log=$new_log"
    exit 0
fi

test -z "$(pidof crosvm 2>/dev/null || true)"
install_crosvm "$backup" "$old_hash"
start_config "$rollback_config" "$rollback_log"
echo "New crosvm failed to remain alive; previous crosvm restored and rollback launch requested" >&2
exit 1
