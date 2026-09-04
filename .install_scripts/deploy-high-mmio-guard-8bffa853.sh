#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
droidvm=$app_root/bin/droidvm
python=/data/data/com.termux/files/usr/bin/python3
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/high-mmio-guard-8bffa853
start_existing=$stage/droidvm-start-existing.py
config=/data/local/tmp/droidvm-native-working-58068-retry-458ffb2c.json
socket=$app_root/run/s-native-58068-shm-retry-458ffb2c.sock
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
backup_dir=/data/local/tmp/droidvm-sda17/back/host
console_prefix=$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493
console_archive=$backup_dir/console-458ffb2c-pre-high-mmio-guard
log=/data/local/tmp/crosvm-native-58070-high-mmio-guard-8bffa853.log
expected_name=s-native-58068-shm-retry-458ffb2c
expected_id=458ffb2c-a1e9-4830-bec5-4e1fe323b493

old_crosvm_hash=d964506092a810e66b67c4a2692f76bd32040387781289781b44b4088c65a0b5
new_crosvm_hash=8bffa853a6601e68ba795d039474bf2d1fc56683e84ed122c4dd503c91f2dec4

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
    while [ "$attempt" -lt 45 ]; do
        status=$("$droidvm" status "$expected_id" 2>/dev/null || true)
        case "$status" in
            *'"state" : "stopped"'*|*'"state":"stopped"'*) break ;;
        esac
        attempt=$((attempt + 1))
        sleep 1
    done
    test "$attempt" -lt 45

    rm -f "$log"
    "$python" "$start_existing" "$expected_id" >"$log" 2>&1
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

archive_console()
{
    destination=$backup_dir/high-mmio-guard-8bffa853-$1
    mkdir -p "$destination"
    for suffix in stderr stdio uart; do
        source_file=$console_prefix\_$suffix.log
        if [ -f "$source_file" ]; then
            cp -p "$source_file" "$destination/$suffix.log"
        fi
    done
    cp -p "$log" "$destination/launcher.log" 2>/dev/null || true
    sync
}

for path in "$app_crosvm" "$droidvm" "$config" "$image" \
    "$stage/crosvm" "$python" "$start_existing"; do
    test -f "$path"
done
test -x "$app_crosvm"
test -x "$droidvm"
test "$(hash_file "$app_crosvm")" = "$old_crosvm_hash"
test "$(hash_file "$stage/crosvm")" = "$new_crosvm_hash"
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

install_file "$stage/crosvm" "$app_crosvm" "$new_crosvm_hash"
start_vm
if wait_for_vm; then
    echo "crosvm_hash=$(hash_file "$app_crosvm")"
    echo "config=$config"
    echo "log=$log"
    exit 0
fi

archive_console failed-attempt-1
test -z "$(pidof crosvm 2>/dev/null || true)"
sleep 3
start_vm
if wait_for_vm; then
    echo "crosvm_hash=$(hash_file "$app_crosvm")"
    echo "config=$config"
    echo "log=$log"
    echo "startup_retry=1"
    exit 0
fi

archive_console failed-attempt-2
test -z "$(pidof crosvm 2>/dev/null || true)"
install_file "$backup_dir/crosvm-$old_crosvm_hash" "$app_crosvm" "$old_crosvm_hash"
start_vm
echo "High-MMIO guard runtime failed to remain alive; previous crosvm restored" >&2
exit 1
