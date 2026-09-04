#!/system/bin/sh
set -eu

mode=${1:-}
case "$mode" in
    --validate-only|--deploy) ;;
    *) echo "usage: $0 --validate-only|--deploy" >&2; exit 2 ;;
esac

if [ "$(id -u)" != "0" ]; then
    exec su -c "/system/bin/sh $0 $mode"
fi

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_virgl=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
library_path=$app_root/usr/lib:$app_root/lib
python=/data/data/com.termux/files/usr/bin/python3
stage=/data/data/com.termux/files/home/crosvm-vnc-recovery-94c88c2a
start_existing=$stage/droidvm-start-existing.py
stop_existing=$stage/droidvm-stop-existing.py
vm_id=a5b6863f-6bbf-4af4-97f3-2a4653f6edb7
backup_dir=/data/local/tmp/droidvm-crosvm-backups/vnc-recovery-94c88c2a
old_crosvm_hash=bbd5eacd4d430e68c2fe497be681d1367f590a07d86eb7b60179d8a002ef53dc
new_crosvm_hash=d1ed68aa4b5145cee0536c63f0774c944599ec9bca9261e1faba2d2e863e1e6a
virgl_hash=77a99067ec8fde12a8261865821f9f6da0f024c5f176663c83dd2214fdc145af

hash_file() { sha256sum "$1" | awk '{print $1}'; }

daemon_state() {
    LD_LIBRARY_PATH="$library_path" "$droidvm" status "$vm_id" 2>/dev/null || true
}

wait_no_vm() {
    n=0
    while pidof crosvm >/dev/null 2>&1; do
        n=$((n + 1)); test "$n" -lt 90 || return 1; sleep 1
    done
}

wait_stopped() {
    n=0
    while [ "$n" -lt 90 ]; do
        state=$(daemon_state)
        case "$state" in *'"state" : "stopped"'*|*'"state":"stopped"'*) return 0;; esac
        n=$((n + 1)); sleep 1
    done
    return 1
}

check_cmdline() {
    cmd=$1
    for fragment in "--name s" "--mem 2048" \
        "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2" \
        "context-types=drm" "udmabuf=true" "fixed-blob-mapping=true" \
        "pci-bar-size=8388608" "--vnc-server" "screen=simplefb"; do
        case "$cmd" in *"$fragment"*) ;; *) return 1;; esac
    done
}

has_vnc_listener() {
    pid=$1
    awk '$2 ~ /:170C$/ && $4 == "0A" { found=1 } END { exit !found }' \
        "/proc/$pid/net/tcp" "/proc/$pid/net/tcp6" 2>/dev/null
}

wait_running() {
    expected_crosvm=$1
    require_vnc=$2
    n=0
    pid=
    cmd=
    while [ "$n" -lt 120 ]; do
        set -- $(pidof crosvm 2>/dev/null || true)
        if [ "$#" -eq 1 ]; then
            pid=$1
            cmd=$(xargs -0 <"/proc/$pid/cmdline")
            test "$(readlink /proc/$pid/exe)" = "$app_crosvm"
            check_cmdline "$cmd"
            test "$(hash_file "$app_crosvm")" = "$expected_crosvm"
            test "$(hash_file "$app_virgl")" = "$virgl_hash"
            break
        fi
        test "$#" -eq 0 || return 2
        n=$((n + 1)); sleep 1
    done
    test -n "$pid"

    n=0
    while [ "$n" -lt 20 ]; do
        test "$(pidof crosvm 2>/dev/null || true)" = "$pid"
        n=$((n + 1)); sleep 1
    done

    if [ "$require_vnc" -eq 1 ]; then
        n=0
        while [ "$n" -lt 20 ]; do
            has_vnc_listener "$pid" && break
            n=$((n + 1)); sleep 1
        done
        test "$n" -lt 20
    fi

    echo "CROSVM_PID=$pid"
    echo "CMDLINE=$cmd"
    echo "VNC_LISTENER_REQUIRED=$require_vnc"
}

install_crosvm() {
    src=$1
    expected=$2
    tmp=$app_crosvm.94c88c2a-new
    owner=$(stat -c '%u:%g' "$app_crosvm")
    perms=$(stat -c '%a' "$app_crosvm")
    context=$(ls -Zd "$app_crosvm" | awk '{print $1}')
    rm -f "$tmp"
    cp "$src" "$tmp"
    chown "$owner" "$tmp"
    chmod "$perms" "$tmp"
    chcon "$context" "$tmp"
    test "$(hash_file "$tmp")" = "$expected"
    mv -f "$tmp" "$app_crosvm"
    sync
    test "$(hash_file "$app_crosvm")" = "$expected"
}

archive_logs() {
    archive=$backup_dir/$1
    if [ ! -d "$archive" ]; then
        mkdir -p "$archive"
        for suffix in stderr stdio serial1; do
            source_log=$app_root/cache/console_${vm_id}_${suffix}.log
            if [ -f "$source_log" ]; then cp -p "$source_log" "$archive/"; fi
        done
        sync
    fi
}

for file in "$app_crosvm" "$app_virgl" "$droidvm" "$start_existing" \
    "$stop_existing" "$stage/crosvm"; do test -f "$file"; done
set -- $(pidof crosvm 2>/dev/null)
test "$#" -eq 1
pid=$1
exe=$(readlink "/proc/$pid/exe")
case "$exe" in "$app_crosvm"|"$app_crosvm (deleted)") ;; *) exit 1;; esac
test "$(hash_file "/proc/$pid/exe")" = "$old_crosvm_hash"
cmd=$(xargs -0 <"/proc/$pid/cmdline")
check_cmdline "$cmd"
case "$(daemon_state)" in *'"state" : "running"'*|*'"state":"running"'*) ;; *) exit 1;; esac
test "$(hash_file "$app_crosvm")" = "$old_crosvm_hash"
test "$(hash_file "$app_virgl")" = "$virgl_hash"
test "$(hash_file "$stage/crosvm")" = "$new_crosvm_hash"

mkdir -p "$backup_dir"
backup_crosvm=$backup_dir/crosvm-$old_crosvm_hash
if [ -f "$backup_crosvm" ]; then
    test "$(hash_file "$backup_crosvm")" = "$old_crosvm_hash"
else
    cp -p "$app_crosvm" "$backup_crosvm"
    sync
    test "$(hash_file "$backup_crosvm")" = "$old_crosvm_hash"
fi

if [ "$mode" = "--validate-only" ]; then
    echo "VALIDATION_ONLY=1"
    echo "CROSVM_PID=$pid"
    echo "CMDLINE=$cmd"
    echo "ACTIVE_CROSVM_SHA256=$old_crosvm_hash"
    echo "STAGED_CROSVM_SHA256=$new_crosvm_hash"
    echo "ACTIVE_VIRGL_SHA256=$virgl_hash"
    exit 0
fi

archive_logs logs-before-$new_crosvm_hash
rollback=1
trap 'status=$?; if [ "$rollback" -eq 1 ]; then set +e; archive_logs logs-failed-candidate-$new_crosvm_hash || true; "$python" "$stop_existing" "$vm_id" >/dev/null 2>&1 || true; wait_no_vm || true; install_crosvm "$backup_crosvm" "$old_crosvm_hash" || true; wait_stopped || true; "$python" "$start_existing" "$vm_id" >/dev/null 2>&1 || true; wait_running "$old_crosvm_hash" 0 || true; fi; exit "$status"' EXIT

"$python" "$stop_existing" "$vm_id"
wait_no_vm
wait_stopped
install_crosvm "$stage/crosvm" "$new_crosvm_hash"
"$python" "$start_existing" "$vm_id"
wait_running "$new_crosvm_hash" 1
rollback=0
trap - EXIT
echo "ACTIVE_CROSVM_SHA256=$new_crosvm_hash"
echo "ACTIVE_VIRGL_SHA256=$virgl_hash"
echo "VNC_LISTENER=1"
echo "DEPLOYED=1"
