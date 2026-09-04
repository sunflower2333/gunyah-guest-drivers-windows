#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    exec su -c "/system/bin/sh $0"
fi

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_virgl=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
python=/data/data/com.termux/files/usr/bin/python3
library_path=$app_root/usr/lib:$app_root/lib
start_existing=/data/data/com.termux/files/home/crosvm-context-diagnostics-5f60c6e6/droidvm-start-existing.py
stop_existing=/data/data/com.termux/files/home/crosvm-context-diagnostics-5f60c6e6/droidvm-stop-existing.py
vm_id=a5b6863f-6bbf-4af4-97f3-2a4653f6edb7
backup_dir=/data/local/tmp/droidvm-crosvm-backups/context-diagnostics-e7433abd
expected_crosvm=bbd5eacd4d430e68c2fe497be681d1367f590a07d86eb7b60179d8a002ef53dc
expected_virgl=77a99067ec8fde12a8261865821f9f6da0f024c5f176663c83dd2214fdc145af

hash_file() { sha256sum "$1" | awk '{print $1}'; }

install_file() {
    source_file=$1
    target_file=$2
    temporary=$target_file.rollback-new
    owner=$(stat -c '%u:%g' "$target_file")
    perms=$(stat -c '%a' "$target_file")
    context=$(ls -Zd "$target_file" | awk '{print $1}')
    rm -f "$temporary"
    cp "$source_file" "$temporary"
    chown "$owner" "$temporary"
    chmod "$perms" "$temporary"
    chcon "$context" "$temporary"
    mv -f "$temporary" "$target_file"
    sync
    test "$(hash_file "$target_file")" = "$3"
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
        state=$(LD_LIBRARY_PATH="$library_path" "$droidvm" status "$vm_id" 2>/dev/null || true)
        case "$state" in *'"state" : "stopped"'*|*'"state":"stopped"'*) return 0 ;; esac
        n=$((n + 1)); sleep 1
    done
    return 1
}

check_cmdline() {
    cmd=$1
    for fragment in "--name s" "--mem 2048" \
        "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2" \
        "context-types=drm" "udmabuf=true" "fixed-blob-mapping=true" \
        "pci-bar-size=8388608"; do
        case "$cmd" in *"$fragment"*) ;; *) return 1 ;; esac
    done
}

wait_running() {
    n=0
    pid=
    while [ "$n" -lt 120 ]; do
        set -- $(pidof crosvm 2>/dev/null || true)
        if [ "$#" -eq 1 ]; then
            pid=$1
            cmd=$(xargs -0 < "/proc/$pid/cmdline")
            test "$(readlink /proc/$pid/exe)" = "$app_crosvm"
            check_cmdline "$cmd"
            test "$(hash_file "$app_crosvm")" = "$expected_crosvm"
            test "$(hash_file "$app_virgl")" = "$expected_virgl"
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
    echo "CROSVM_PID=$pid"
    echo "CMDLINE=$cmd"
}

test -f "$backup_dir/crosvm-$expected_crosvm"
test -f "$backup_dir/libvirglrenderer.so-$expected_virgl"
test -f "$start_existing"
test -f "$stop_existing"
"$python" "$stop_existing" "$vm_id" >/dev/null 2>&1 || true
wait_no_vm
wait_stopped
install_file "$backup_dir/crosvm-$expected_crosvm" "$app_crosvm" "$expected_crosvm"
install_file "$backup_dir/libvirglrenderer.so-$expected_virgl" "$app_virgl" "$expected_virgl"
"$python" "$start_existing" "$vm_id"
wait_running
echo "ROLLED_BACK=1"
