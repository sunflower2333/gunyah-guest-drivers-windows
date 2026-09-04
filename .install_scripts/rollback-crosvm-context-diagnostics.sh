#!/system/bin/sh
set -eu

mode=${1:-}
if [ "$mode" != "--rollback" ]; then
    echo "usage: $0 --rollback" >&2
    exit 2
fi

if [ "$(id -u)" != "0" ]; then
    exec su -c "/system/bin/sh $0 $mode"
fi

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_virgl=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
python=/data/data/com.termux/files/usr/bin/python3
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/crosvm-context-diagnostics-5f60c6e6
start_existing=$stage/droidvm-start-existing.py
stop_existing=$stage/droidvm-stop-existing.py
vm_id=a5b6863f-6bbf-4af4-97f3-2a4653f6edb7
backup_dir=/data/local/tmp/droidvm-crosvm-backups/context-diagnostics-20260831
archive_dir=$backup_dir/diagnostic-logs
console_prefix=$app_root/cache/console_$vm_id
old_crosvm_hash=35552b033fcd7a4200013c21a0974956a75a3f3dcbfcc442220708ecd8637ba0
old_virgl_hash=18316a28baae637629c12319135313c5a33d0ce75995edb5a1087dc399218994
new_crosvm_hash=5f60c6e652fe9c2398f24e12424274f554520b763a98fcb295bae06a26aea03a
new_virgl_hash=f980efd60d71cc2c7717d826b63ccb7b0247563e8831f2cf51db289b1d7fad4c
backup_crosvm=$backup_dir/crosvm-$old_crosvm_hash
backup_virgl=$backup_dir/libvirglrenderer.so-$old_virgl_hash

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

install_file()
{
    source_file=$1
    target_file=$2
    expected_hash=$3
    temporary=$target_file.context-diagnostic-rollback
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

wait_no_vm()
{
    attempt=0
    while pidof crosvm >/dev/null 2>&1; do
        attempt=$((attempt + 1))
        test "$attempt" -lt 60
        sleep 1
    done
}

wait_daemon_stopped()
{
    attempt=0
    while [ "$attempt" -lt 60 ]; do
        status=$(LD_LIBRARY_PATH="$library_path" "$droidvm" status "$vm_id" 2>/dev/null || true)
        case "$status" in
            *'"state" : "stopped"'*|*'"state":"stopped"'*) return 0 ;;
        esac
        attempt=$((attempt + 1))
        sleep 1
    done
    return 1
}

for required in "$app_crosvm" "$app_virgl" "$droidvm" "$python" \
    "$start_existing" "$stop_existing" "$backup_crosvm" "$backup_virgl"; do
    test -f "$required"
done

set -- $(pidof crosvm)
test "$#" -eq 1
test "$(hash_file "$app_crosvm")" = "$new_crosvm_hash"
test "$(hash_file "$app_virgl")" = "$new_virgl_hash"
cmdline=$(xargs -0 < "/proc/$1/cmdline")
case "$cmdline" in
    *"--name s"*"--mem 2048"*"win11-droidvm-final-comp.qcow2"*) ;;
    *) echo "refusing to stop unexpected crosvm: $cmdline" >&2; exit 3 ;;
esac

mkdir -p "$archive_dir"
for suffix in stderr stdio serial1; do
    source_log=$console_prefix\_$suffix.log
    if [ -f "$source_log" ]; then
        cp -p "$source_log" "$archive_dir/$suffix.log"
    fi
done
sync

"$python" "$stop_existing" "$vm_id"
wait_no_vm
wait_daemon_stopped
install_file "$backup_virgl" "$app_virgl" "$old_virgl_hash"
install_file "$backup_crosvm" "$app_crosvm" "$old_crosvm_hash"
"$python" "$start_existing" "$vm_id"

attempt=0
pid=
while [ "$attempt" -lt 90 ]; do
    set -- $(pidof crosvm 2>/dev/null || true)
    if [ "$#" -eq 1 ]; then
        pid=$1
        break
    fi
    test "$#" -eq 0
    attempt=$((attempt + 1))
    sleep 1
done
test -n "$pid"
test "$(readlink /proc/$pid/exe)" = "$app_crosvm"
grep -F "$app_virgl" "/proc/$pid/maps" >/dev/null
cmdline=$(xargs -0 < "/proc/$pid/cmdline")
case "$cmdline" in
    *"--name s"*"--mem 2048"*"win11-droidvm-final-comp.qcow2"*"context-types=drm"*"udmabuf=true"*"fixed-blob-mapping=true"*"pci-bar-size=8388608"*) ;;
    *) echo "unexpected restored crosvm: $cmdline" >&2; exit 4 ;;
esac
sleep 20
test "$(pidof crosvm 2>/dev/null || true)" = "$pid"
test "$(hash_file "$app_crosvm")" = "$old_crosvm_hash"
test "$(hash_file "$app_virgl")" = "$old_virgl_hash"
echo "CROSVM_PID=$pid"
echo "ACTIVE_CROSVM_SHA256=$old_crosvm_hash"
echo "ACTIVE_VIRGL_SHA256=$old_virgl_hash"
echo "ROLLBACK_COMPLETE=1"
