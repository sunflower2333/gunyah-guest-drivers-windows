#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
droidvm=$app_root/bin/droidvm
config=/data/local/tmp/droidvm-lowbar-live-config.json
termux_python=/data/data/com.termux/files/usr/bin/python3
start_existing=/data/data/com.termux/files/home/crosvm-lowbar-58ef7458/droidvm-start-existing.py
vm_id=f227cc7b-2a94-4a17-ab7d-54d4f2a1b5b3
vm_name=s-remote-test
old_hash=4a9182c33e487372dd4189ca5556aaec49f48cdb8da35e7882fded88234e4a4f
start_log=/data/local/tmp/droidvm-after-lowbar-rollback-start.log

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

test -z "$(pidof crosvm 2>/dev/null || true)"
test -x "$app_crosvm"
test -x "$droidvm"
test -x "$termux_python"
test -f "$start_existing"
test -f "$config"
test "$(hash_file "$app_crosvm")" = "$old_hash"
grep -F '"id":"f227cc7b-2a94-4a17-ab7d-54d4f2a1b5b3"' "$config" >/dev/null
grep -F '"name":"s-remote-test"' "$config" >/dev/null
grep -F '"memory_mb":2048' "$config" >/dev/null
grep -F '"protected_vm":"pseudo_unprotected"' "$config" >/dev/null
grep -F '"display_backend":"virtio_gpu"' "$config" >/dev/null
grep -F '"gpu_udmabuf":true' "$config" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$config" >/dev/null
grep -F '/storage/emulated/0/win11-droidvm-final-comp.qcow2' "$config" >/dev/null

: >"$start_log"
if ! "$termux_python" "$start_existing" "$vm_id" >>"$start_log" 2>&1; then
    if ! grep -F 'VM not found' "$start_log" >/dev/null; then
        cat "$start_log" >&2
        exit 1
    fi
    "$droidvm" start "$(cat "$config")" >>"$start_log" 2>&1
fi

attempt=0
pid=
while [ "$attempt" -lt 90 ]; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
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
    echo "Unexpected crosvm after rollback start: $cmdline" >&2
    exit 1
fi

sleep 15
if [ "$(pidof crosvm 2>/dev/null || true)" != "$pid" ]; then
    echo "crosvm $pid exited during the stability window" >&2
    exit 1
fi

echo "crosvm_pid=$pid"
echo "crosvm_hash=$old_hash"
echo "$cmdline"
