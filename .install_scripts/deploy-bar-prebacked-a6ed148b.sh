#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_renderer=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/bar-prebacked-a6ed148b
backup_dir=/data/local/tmp/droidvm-sda17/back/host
source_config=/data/local/tmp/droidvm-native-working-58062-817db57c.json
new_config=/data/local/tmp/droidvm-native-working-58063-40d515a6.json
rollback_config=/data/local/tmp/droidvm-native-working-58062-rollback-65967948.json
current_socket=$app_root/run/s-native-58062-r817db57c.sock
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
new_name=s-native-58063-bar8m-40d515a6
rollback_name=s-native-58062-rollback-65967948
new_log=/data/local/tmp/crosvm-native-58063-40d515a6.log
rollback_log=/data/local/tmp/crosvm-native-58062-rollback-65967948.log

old_crosvm_hash=e6004a9f8b31b327dd87b9ab9e5d68a83637392c94cc8e3db17b9d96bcb9d789
old_renderer_hash=b395fb8225493b10333e41b9145679d451e3d1c4e38dd4dcbb8baac04428a94a
new_crosvm_hash=a6ed148b5ce71b0b2dec866edc4adcc997438985ec66a6535b96a1c1094dda48
new_renderer_hash=50bf1dc1a5e75ea45a56a0b13c1c7e7b10a205856c0de8dda19140471ef946f6

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

backup_file()
{
    source_file=$1
    backup_file=$2
    expected_hash=$3
    if [ -f "$backup_file" ]; then
        test "$(hash_file "$backup_file")" = "$expected_hash"
    else
        cp -p "$source_file" "$backup_file"
        sync
        test "$(hash_file "$backup_file")" = "$expected_hash"
    fi
}

install_file()
{
    source_file=$1
    target_file=$2
    expected_hash=$3
    temporary=$target_file.deploy-new
    owner=$(stat -c '%u:%g' "$target_file")
    mode=$(stat -c '%a' "$target_file")
    context=$(ls -Zd "$target_file" | awk '{print $1}')

    rm -f "$temporary"
    cp "$source_file" "$temporary"
    chown "$owner" "$temporary"
    chmod "$mode" "$temporary"
    chcon "$context" "$temporary"
    test "$(hash_file "$temporary")" = "$expected_hash"
    mv -f "$temporary" "$target_file"
    sync
    test "$(hash_file "$target_file")" = "$expected_hash"
}

start_config()
{
    config=$1
    log=$2
    rm -f "$log"
    nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
    echo "launcher_pid=$!"
}

for path in "$app_crosvm" "$app_renderer" "$droidvm" "$source_config" \
    "$expected_image" "$stage/crosvm" "$stage/libvirglrenderer.so" \
    "$stage/native-58063-bar8m.json" "$stage/native-58062-rollback.json"; do
    test -f "$path"
done
test -x "$app_crosvm"
test -x "$droidvm"
test "$(hash_file "$app_crosvm")" = "$old_crosvm_hash"
test "$(hash_file "$app_renderer")" = "$old_renderer_hash"
test "$(hash_file "$stage/crosvm")" = "$new_crosvm_hash"
test "$(hash_file "$stage/libvirglrenderer.so")" = "$new_renderer_hash"

grep -F '"memory_mb":2048' "$source_config" >/dev/null
grep -F '"gpu_udmabuf":true' "$source_config" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$source_config" >/dev/null
grep -F "$expected_image" "$source_config" >/dev/null
grep -F '"id":"40d515a6-8d60-4739-82d3-f413e23ad5ea"' "$stage/native-58063-bar8m.json" >/dev/null
grep -F '"gpu_pci_bar_size":8388608' "$stage/native-58063-bar8m.json" >/dev/null
grep -F '"id":"65967948-e48b-44ca-906a-1130832af3fe"' "$stage/native-58062-rollback.json" >/dev/null
if grep -F '"gpu_pci_bar_size"' "$stage/native-58062-rollback.json" >/dev/null; then
    echo "rollback config unexpectedly overrides the BAR" >&2
    exit 1
fi

pid=$(pidof crosvm 2>/dev/null || true)
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
case "$cmdline" in
    *"--name s-native-58062-r817db57c"*"--mem 2048"*"$expected_image"*) ;;
    *)
        echo "Refusing to stop unexpected crosvm: $cmdline" >&2
        exit 1
        ;;
esac
case "$cmdline" in
    *"--pre-alloc drm-host-mb=8"*"pci-bar-size=4294967296"*) ;;
    *)
        echo "Refusing unexpected GPU memory configuration: $cmdline" >&2
        exit 1
        ;;
esac

mkdir -p "$backup_dir"
backup_file "$app_crosvm" "$backup_dir/crosvm-$old_crosvm_hash" "$old_crosvm_hash"
backup_file "$app_renderer" "$backup_dir/libvirglrenderer-$old_renderer_hash.so" "$old_renderer_hash"
cp "$stage/native-58063-bar8m.json" "$new_config"
cp "$stage/native-58062-rollback.json" "$rollback_config"
chmod 600 "$new_config" "$rollback_config"
sync

LD_LIBRARY_PATH="$library_path" "$app_crosvm" stop "$current_socket"
attempt=0
while pidof crosvm >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 45 ]; then
        echo "crosvm did not stop within 45 seconds" >&2
        exit 1
    fi
    sleep 1
done

install_file "$stage/libvirglrenderer.so" "$app_renderer" "$new_renderer_hash"
install_file "$stage/crosvm" "$app_crosvm" "$new_crosvm_hash"
start_config "$new_config" "$new_log"

attempt=0
while :; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        test "$(echo "$pid" | wc -w)" -eq 1
        cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
        case "$cmdline" in
            *"--name $new_name"*"--mem 2048"*"$expected_image"*) ;;
            *)
                echo "Unexpected crosvm after deployment: $cmdline" >&2
                exit 1
                ;;
        esac
        case "$cmdline" in
            *"--pre-alloc drm-host-mb=8"*"pci-bar-size=8388608"*)
                echo "crosvm_pid=$pid"
                echo "crosvm_hash=$(hash_file "$app_crosvm")"
                echo "renderer_hash=$(hash_file "$app_renderer")"
                echo "config=$new_config"
                echo "log=$new_log"
                echo "$cmdline"
                exit 0
                ;;
            *)
                echo "Unexpected GPU memory configuration after deployment: $cmdline" >&2
                exit 1
                ;;
        esac
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 60 ]; then
        echo "New runtime did not start within 60 seconds; restoring both host files" >&2
        break
    fi
    sleep 1
done

install_file "$backup_dir/libvirglrenderer-$old_renderer_hash.so" "$app_renderer" "$old_renderer_hash"
install_file "$backup_dir/crosvm-$old_crosvm_hash" "$app_crosvm" "$old_crosvm_hash"
start_config "$rollback_config" "$rollback_log"
echo "Previous host runtime restored; rollback launch requested as $rollback_name" >&2
exit 1
