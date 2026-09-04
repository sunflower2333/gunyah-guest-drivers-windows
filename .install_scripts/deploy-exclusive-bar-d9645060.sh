#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_renderer=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
python=/data/data/com.termux/files/usr/bin/python3
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/exclusive-bar-d9645060
start_existing=$stage/droidvm-start-existing.py
config=/data/local/tmp/droidvm-native-working-58068-retry-458ffb2c.json
socket=$app_root/run/s-native-58068-shm-retry-458ffb2c.sock
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
backup_dir=/data/local/tmp/droidvm-sda17/back/host
console_prefix=$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493
console_archive=$backup_dir/console-458ffb2c-pre-exclusive-bar
log=/data/local/tmp/crosvm-native-58069-exclusive-bar-d9645060.log
expected_name=s-native-58068-shm-retry-458ffb2c
expected_id=458ffb2c-a1e9-4830-bec5-4e1fe323b493

old_crosvm_hash=27c7d486e985b3c16cd08c1c201bd0d36d47b8052c9eea7611f258e9754d4636
old_renderer_hash=50bf1dc1a5e75ea45a56a0b13c1c7e7b10a205856c0de8dda19140471ef946f6
new_crosvm_hash=d964506092a810e66b67c4a2692f76bd32040387781289781b44b4088c65a0b5
new_renderer_hash=68fd5d223e505d4dff6a3822bb41cb7ba9aeb7a1ec9d2a9a733ec9deefe6909c

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

backup_file()
{
    source_file=$1
    destination=$2
    expected_hash=$3
    if [ -f "$destination" ]; then
        test "$(hash_file "$destination")" = "$expected_hash"
    else
        cp -p "$source_file" "$destination"
        sync
        test "$(hash_file "$destination")" = "$expected_hash"
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

start_vm()
{
    attempt=0
    registered=0
    while [ "$attempt" -lt 45 ]; do
        if status=$("$droidvm" status "$expected_id" 2>/dev/null); then
            registered=1
        else
            registered=0
            break
        fi
        case "$status" in
            *'"state" : "stopped"'*) break ;;
            *'"state":"stopped"'*) break ;;
        esac
        attempt=$((attempt + 1))
        sleep 1
    done

    rm -f "$log"
    if [ "$registered" -eq 1 ]; then
        if [ "$attempt" -ge 45 ]; then
            echo "registered VM did not reach stopped state" >"$log"
            return 1
        fi
        "$python" "$start_existing" "$expected_id" >"$log" 2>&1
        echo "started_registered_vm=$expected_id"
    else
        nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
        echo "launcher_pid=$!"
    fi
}

wait_for_vm()
{
    attempt=0
    pid=
    while [ "$attempt" -lt 60 ]; do
        pid=$(pidof crosvm 2>/dev/null || true)
        if [ -n "$pid" ]; then
            test "$(echo "$pid" | wc -w)" -eq 1
            cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
            case "$cmdline" in
                *"--name $expected_name"*"--mem 2048"*"$image"*) ;;
                *) return 2 ;;
            esac
            case "$cmdline" in
                *"--pre-alloc drm-host-mb=8"*"udmabuf=true"*"pci-bar-size=8388608"*) break ;;
                *) return 2 ;;
            esac
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    test -n "$pid" || return 1

    stable=0
    while [ "$stable" -lt 20 ]; do
        test "$(pidof crosvm 2>/dev/null || true)" = "$pid" || return 1
        stable=$((stable + 1))
        sleep 1
    done
    echo "crosvm_pid=$pid"
    echo "$cmdline"
}

for path in "$app_crosvm" "$app_renderer" "$droidvm" "$config" "$image" \
    "$stage/crosvm" "$stage/libvirglrenderer.so" "$python" "$start_existing"; do
    test -f "$path"
done
test -x "$app_crosvm"
test -x "$droidvm"
test "$(hash_file "$app_crosvm")" = "$old_crosvm_hash"
test "$(hash_file "$app_renderer")" = "$old_renderer_hash"
test "$(hash_file "$stage/crosvm")" = "$new_crosvm_hash"
test "$(hash_file "$stage/libvirglrenderer.so")" = "$new_renderer_hash"
grep -F '"memory_mb":2048' "$config" >/dev/null
grep -F '"gpu_udmabuf":true' "$config" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$config" >/dev/null
grep -F '"gpu_pci_bar_size":8388608' "$config" >/dev/null
grep -F "\"id\":\"$expected_id\"" "$config" >/dev/null
grep -F "$image" "$config" >/dev/null

pid=$(pidof crosvm 2>/dev/null || true)
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
case "$cmdline" in
    *"--name $expected_name"*"--mem 2048"*"$image"*) ;;
    *) echo "Refusing to stop unexpected crosvm: $cmdline" >&2; exit 1 ;;
esac

mkdir -p "$backup_dir" "$console_archive"
backup_file "$app_crosvm" "$backup_dir/crosvm-$old_crosvm_hash" "$old_crosvm_hash"
backup_file "$app_renderer" "$backup_dir/libvirglrenderer-$old_renderer_hash.so" "$old_renderer_hash"

LD_LIBRARY_PATH="$library_path" "$app_crosvm" stop "$socket"
attempt=0
while pidof crosvm >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 45 ]; then
        echo "crosvm did not stop within 45 seconds" >&2
        exit 1
    fi
    sleep 1
done

for suffix in stderr stdio uart; do
    old_log=$console_prefix\_$suffix.log
    if [ -f "$old_log" ]; then
        cp -p "$old_log" "$console_archive/$suffix.log"
        rm -f "$old_log"
    fi
done
sync

install_file "$stage/libvirglrenderer.so" "$app_renderer" "$new_renderer_hash"
install_file "$stage/crosvm" "$app_crosvm" "$new_crosvm_hash"
start_vm
if wait_for_vm; then
    echo "crosvm_hash=$(hash_file "$app_crosvm")"
    echo "renderer_hash=$(hash_file "$app_renderer")"
    echo "config=$config"
    echo "log=$log"
    exit 0
fi

test -z "$(pidof crosvm 2>/dev/null || true)"
install_file "$backup_dir/libvirglrenderer-$old_renderer_hash.so" "$app_renderer" "$old_renderer_hash"
install_file "$backup_dir/crosvm-$old_crosvm_hash" "$app_crosvm" "$old_crosvm_hash"
start_vm
echo "Exclusive-BAR runtime failed to remain alive; previous host runtime restored" >&2
exit 1
