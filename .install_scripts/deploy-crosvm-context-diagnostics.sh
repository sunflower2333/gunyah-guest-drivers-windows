#!/system/bin/sh
set -eu

mode=${1:-}
case "$mode" in
    --deploy|--validate-only) ;;
    *) echo "usage: $0 --validate-only|--deploy" >&2; exit 2 ;;
esac

if [ "$(id -u)" != "0" ]; then
    exec su -c "/system/bin/sh $0 $mode"
fi

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_virgl=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
socket=$app_root/run/s.sock
python=/data/data/com.termux/files/usr/bin/python3
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/crosvm-context-diagnostics-5f60c6e6
start_existing=$stage/droidvm-start-existing.py
stop_existing=$stage/droidvm-stop-existing.py
stage_validator=$stage/stage-crosvm-context-diagnostics.sh
vm_id=a5b6863f-6bbf-4af4-97f3-2a4653f6edb7
backup_dir=/data/local/tmp/droidvm-crosvm-backups/context-diagnostics-20260831
archive_dir=$backup_dir/pre-diagnostic-logs
console_prefix=$app_root/cache/console_$vm_id
old_crosvm_hash=35552b033fcd7a4200013c21a0974956a75a3f3dcbfcc442220708ecd8637ba0
old_virgl_hash=18316a28baae637629c12319135313c5a33d0ce75995edb5a1087dc399218994
new_crosvm_hash=5f60c6e652fe9c2398f24e12424274f554520b763a98fcb295bae06a26aea03a
new_virgl_hash=f980efd60d71cc2c7717d826b63ccb7b0247563e8831f2cf51db289b1d7fad4c
backup_crosvm=$backup_dir/crosvm-$old_crosvm_hash
backup_virgl=$backup_dir/libvirglrenderer.so-$old_virgl_hash
rollback_needed=0

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

install_file()
{
    source_file=$1
    target_file=$2
    expected_hash=$3
    temporary=$target_file.context-diagnostic-new
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
        if [ "$attempt" -ge 60 ]; then
            return 1
        fi
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

check_cmdline()
{
    cmdline=$1
    for fragment in \
        "--name s" \
        "--mem 2048" \
        "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2" \
        "context-types=drm" \
        "udmabuf=true" \
        "fixed-blob-mapping=true" \
        "pci-bar-size=8388608"; do
        case "$cmdline" in
            *"$fragment"*) ;;
            *) return 1 ;;
        esac
    done
}

wait_for_vm()
{
    expected_crosvm_hash=$1
    expected_virgl_hash=$2
    attempt=0
    pid=
    while [ "$attempt" -lt 90 ]; do
        set -- $(pidof crosvm 2>/dev/null || true)
        if [ "$#" -eq 1 ]; then
            pid=$1
            cmdline=$(xargs -0 < "/proc/$pid/cmdline")
            check_cmdline "$cmdline" || return 2
            test "$(readlink /proc/$pid/exe)" = "$app_crosvm" || return 2
            grep -F "$app_virgl" "/proc/$pid/maps" >/dev/null || return 2
            test "$(hash_file "$app_crosvm")" = "$expected_crosvm_hash" || return 2
            test "$(hash_file "$app_virgl")" = "$expected_virgl_hash" || return 2
            break
        fi
        if [ "$#" -gt 1 ]; then
            return 2
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

    echo "CROSVM_PID=$pid"
    echo "CMDLINE=$cmdline"
}

start_vm()
{
    wait_daemon_stopped
    "$python" "$start_existing" "$vm_id"
}

stop_vm()
{
    "$python" "$stop_existing" "$vm_id"
    wait_no_vm
    wait_daemon_stopped
}

rollback_on_exit()
{
    status=$?
    trap - EXIT
    if [ "$rollback_needed" -eq 1 ]; then
        set +e
        rollback_ok=1
        echo "diagnostic deployment failed; restoring both packaged Host files" >&2
        if pidof crosvm >/dev/null 2>&1; then
            "$python" "$stop_existing" "$vm_id" >/dev/null 2>&1 || \
                LD_LIBRARY_PATH="$library_path" "$app_crosvm" stop "$socket" >/dev/null 2>&1 || true
            wait_no_vm || rollback_ok=0
        fi
        if ! pidof crosvm >/dev/null 2>&1; then
            install_file "$backup_virgl" "$app_virgl" "$old_virgl_hash" || rollback_ok=0
            install_file "$backup_crosvm" "$app_crosvm" "$old_crosvm_hash" || rollback_ok=0
            wait_daemon_stopped || rollback_ok=0
            if [ "$rollback_ok" -eq 1 ]; then
                start_vm || rollback_ok=0
                wait_for_vm "$old_crosvm_hash" "$old_virgl_hash" || rollback_ok=0
            fi
        else
            rollback_ok=0
        fi
        if [ "$rollback_ok" -eq 1 ]; then
            echo "ROLLBACK_COMPLETE=1" >&2
        else
            echo "ROLLBACK_INCOMPLETE=1" >&2
        fi
    fi
    exit "$status"
}

trap rollback_on_exit EXIT

for required in "$app_crosvm" "$app_virgl" "$droidvm" "$python" \
    "$start_existing" "$stop_existing" "$stage_validator" \
    "$stage/crosvm" "$stage/libvirglrenderer.so" \
    "$backup_crosvm" "$backup_virgl"; do
    test -f "$required"
done

/system/bin/sh "$stage_validator"
if [ "$mode" = "--validate-only" ]; then
    rollback_needed=0
    trap - EXIT
    echo "DEPLOY_VALIDATION_ONLY=1"
    exit 0
fi

mkdir -p "$archive_dir"
for suffix in stderr stdio serial1; do
    source_log=$console_prefix\_$suffix.log
    if [ -f "$source_log" ]; then
        cp -p "$source_log" "$archive_dir/$suffix.log"
    fi
done
sync

stop_vm
rollback_needed=1
install_file "$stage/libvirglrenderer.so" "$app_virgl" "$new_virgl_hash"
install_file "$stage/crosvm" "$app_crosvm" "$new_crosvm_hash"
start_vm
wait_for_vm "$new_crosvm_hash" "$new_virgl_hash"

rollback_needed=0
trap - EXIT
echo "ACTIVE_CROSVM_SHA256=$new_crosvm_hash"
echo "ACTIVE_VIRGL_SHA256=$new_virgl_hash"
echo "DIAGNOSTIC_DEPLOYED=1"
