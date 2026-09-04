#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
droidvm=$app_root/bin/droidvm
termux_python=/data/data/com.termux/files/usr/bin/python3
stage=/data/data/com.termux/files/home/crosvm-lowbar-58ef7458
staged_crosvm=$stage/crosvm
extract_config=$stage/extract-droidvm-config.py
start_existing=$stage/droidvm-start-existing.py
vm_id=f227cc7b-2a94-4a17-ab7d-54d4f2a1b5b3
vm_name=s-remote-test
socket=$app_root/run/$vm_name.sock
expected_image=/storage/emulated/0/win11-droidvm-final-comp.qcow2
old_hash=4a9182c33e487372dd4189ca5556aaec49f48cdb8da35e7882fded88234e4a4f
new_hash=58ef745806fd30341b2110eed8f07b54eabdeed61b9b62232e736f65cc548a8e
backup_dir=/data/local/tmp/droidvm-crosvm-backups
list_json=/data/local/tmp/droidvm-lowbar-live-list.json
config_json=/data/local/tmp/droidvm-lowbar-live-config.json
start_log=/data/local/tmp/droidvm-lowbar-start.log

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

check_cmdline()
{
    actual_cmdline=$1
    shift
    for expected_fragment in "$@"; do
        case "$actual_cmdline" in
            *"$expected_fragment"*) ;;
            *)
                echo "Missing expected crosvm argument: $expected_fragment" >&2
                return 1
                ;;
        esac
    done
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

start_vm()
{
    : >"$start_log"
    if "$termux_python" "$start_existing" "$vm_id" >>"$start_log" 2>&1; then
        return 0
    fi
    "$droidvm" start "$(cat "$config_json")" >>"$start_log" 2>&1
}

wait_for_vm()
{
    expected_hash=$1
    attempt=0
    while [ "$attempt" -lt 90 ]; do
        pid=$(pidof crosvm 2>/dev/null || true)
        if [ -n "$pid" ]; then
            test "$(echo "$pid" | wc -w)" -eq 1
            cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
            if ! check_cmdline "$cmdline" \
                "--name $vm_name" \
                "--mem 2048" \
                "win11-droidvm-final-comp.qcow2" \
                "--pre-alloc drm-host-mb=8" \
                "--protected-vm-pseudo-unprotected" \
                "udmabuf=true" \
                "fixed-blob-mapping=true" \
                "pci-bar-size=8388608"; then
                echo "Unexpected crosvm after launch: $cmdline" >&2
                return 1
            fi
            if [ "$(hash_file "$app_crosvm")" != "$expected_hash" ]; then
                echo "Installed crosvm hash changed during launch" >&2
                return 1
            fi
            sleep 10
            if [ "$(pidof crosvm 2>/dev/null || true)" != "$pid" ]; then
                echo "crosvm $pid exited during the stability window" >&2
                return 1
            fi
            echo "crosvm_pid=$pid"
            echo "crosvm_hash=$expected_hash"
            echo "$cmdline"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    return 1
}

test -x "$app_crosvm"
test -x "$droidvm"
test -x "$termux_python"
test -f "$staged_crosvm"
test -f "$extract_config"
test -f "$start_existing"
test -S "$socket"
test "$(hash_file "$app_crosvm")" = "$old_hash"
test "$(hash_file "$staged_crosvm")" = "$new_hash"

pid=$(pidof crosvm 2>/dev/null || true)
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
if ! check_cmdline "$cmdline" \
    "--name $vm_name" \
    "--mem 2048" \
    "win11-droidvm-final-comp.qcow2" \
    "--pre-alloc drm-host-mb=8" \
    "--protected-vm-pseudo-unprotected" \
    "udmabuf=true" \
    "fixed-blob-mapping=true" \
    "pci-bar-size=8388608"; then
    echo "Refusing to stop unexpected crosvm: $cmdline" >&2
    exit 1
fi

"$droidvm" list >"$list_json"
"$termux_python" "$extract_config" "$list_json" "$vm_id" "$config_json"
grep -F '"memory_mb":2048' "$config_json" >/dev/null
grep -F '"gpu_udmabuf":true' "$config_json" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$config_json" >/dev/null
grep -F '"protected_vm":"pseudo_unprotected"' "$config_json" >/dev/null
grep -F "$expected_image" "$config_json" >/dev/null

mkdir -p "$backup_dir"
backup=$backup_dir/crosvm-$old_hash
if [ -f "$backup" ]; then
    test "$(hash_file "$backup")" = "$old_hash"
else
    cp -p "$app_crosvm" "$backup"
    sync
    test "$(hash_file "$backup")" = "$old_hash"
fi

"$droidvm" stop "$vm_id"
attempt=0
while pidof crosvm >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 60 ]; then
        echo "crosvm did not stop within 60 seconds" >&2
        exit 1
    fi
    sleep 1
done

install_crosvm "$staged_crosvm" "$new_hash"
if start_vm && wait_for_vm "$new_hash"; then
    exit 0
fi

echo "New crosvm did not remain running; restoring previous executable" >&2
"$droidvm" stop "$vm_id" >/dev/null 2>&1 || true
attempt=0
while pidof crosvm >/dev/null 2>&1 && [ "$attempt" -lt 30 ]; do
    attempt=$((attempt + 1))
    sleep 1
done
test -z "$(pidof crosvm 2>/dev/null || true)"
install_crosvm "$backup" "$old_hash"
start_vm
wait_for_vm "$old_hash"
exit 1
