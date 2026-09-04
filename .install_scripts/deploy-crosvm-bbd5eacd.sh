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
python=/data/data/com.termux/files/usr/bin/python3
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/crosvm-context-diagnostics-bbd5eacd
start_existing=/data/data/com.termux/files/home/crosvm-context-diagnostics-5f60c6e6/droidvm-start-existing.py
stop_existing=/data/data/com.termux/files/home/crosvm-context-diagnostics-5f60c6e6/droidvm-stop-existing.py
vm_id=a5b6863f-6bbf-4af4-97f3-2a4653f6edb7
backup_dir=/data/local/tmp/droidvm-crosvm-backups/context-diagnostics-bbd5eacd
old_crosvm_hash=59c0132b4f9c6b09991fae2a615c4034c65747a1b95b447bf6c81b2c7658fc45
old_virgl_hash=d2220134993645d93a2ad1a1b6981ea18edb55aacf8011b15125b7342c6eefd1
new_crosvm_hash=bbd5eacd4d430e68c2fe497be681d1367f590a07d86eb7b60179d8a002ef53dc
new_virgl_hash=77a99067ec8fde12a8261865821f9f6da0f024c5f176663c83dd2214fdc145af

hash_file() { sha256sum "$1" | awk '{print $1}'; }

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
        case "$state" in *'"state" : "stopped"'*|*'"state":"stopped"'*) return 0;; esac
        n=$((n + 1)); sleep 1
    done
    return 1
}

check_cmdline() {
    cmd=$1
    for f in "--name s" "--mem 2048" "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2" \
        "context-types=drm" "udmabuf=true" "fixed-blob-mapping=true" "pci-bar-size=8388608"; do
        case "$cmd" in *"$f"*) ;; *) return 1;; esac
    done
}

wait_running() {
    expected_crosvm=$1
    expected_virgl=$2
    n=0
    pid=
    while [ "$n" -lt 120 ]; do
        set -- $(pidof crosvm 2>/dev/null || true)
        if [ "$#" -eq 1 ]; then
            pid=$1
            cmd=$(xargs -0 <"/proc/$pid/cmdline")
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

install_file() {
    src=$1; dst=$2; expected=$3
    tmp=$dst.bbd5eacd-new
    owner=$(stat -c '%u:%g' "$dst")
    perms=$(stat -c '%a' "$dst")
    context=$(ls -Zd "$dst" | awk '{print $1}')
    rm -f "$tmp"
    cp "$src" "$tmp"
    chown "$owner" "$tmp"
    chmod "$perms" "$tmp"
    chcon "$context" "$tmp"
    test "$(hash_file "$tmp")" = "$expected"
    mv -f "$tmp" "$dst"
    sync
    test "$(hash_file "$dst")" = "$expected"
}

for f in "$app_crosvm" "$app_virgl" "$droidvm" "$start_existing" "$stop_existing" \
    "$stage/crosvm" "$stage/libvirglrenderer.so"; do test -f "$f"; done
set -- $(pidof crosvm 2>/dev/null)
test "$#" -eq 1
pid=$1
exe=$(readlink /proc/$pid/exe)
case "$exe" in
    "$app_crosvm"|"$app_crosvm (deleted)") ;;
    *) exit 1 ;;
esac
test "$(sha256sum /proc/$pid/exe | awk '{print $1}')" = "$old_crosvm_hash"
cmd=$(xargs -0 <"/proc/$pid/cmdline")
check_cmdline "$cmd"
test "$(hash_file "$app_crosvm")" = "$old_crosvm_hash"
test "$(hash_file "$app_virgl")" = "$old_virgl_hash"
test "$(hash_file "$stage/crosvm")" = "$new_crosvm_hash"
test "$(hash_file "$stage/libvirglrenderer.so")" = "$new_virgl_hash"

mkdir -p "$backup_dir"
backup_crosvm=$backup_dir/crosvm-$old_crosvm_hash
backup_virgl=$backup_dir/libvirglrenderer.so-$old_virgl_hash
if [ -f "$backup_crosvm" ]; then test "$(hash_file "$backup_crosvm")" = "$old_crosvm_hash"; else cp -p "$app_crosvm" "$backup_crosvm"; fi
if [ -f "$backup_virgl" ]; then test "$(hash_file "$backup_virgl")" = "$old_virgl_hash"; else cp -p "$app_virgl" "$backup_virgl"; fi
sync

if [ "$mode" = "--validate-only" ]; then
    echo "VALIDATION_ONLY=1"
    echo "ACTIVE_CROSVM_SHA256=$old_crosvm_hash"
    echo "ACTIVE_VIRGL_SHA256=$old_virgl_hash"
    echo "STAGED_CROSVM_SHA256=$new_crosvm_hash"
    echo "STAGED_VIRGL_SHA256=$new_virgl_hash"
    exit 0
fi

rollback=1
trap 'status=$?; if [ "$rollback" -eq 1 ]; then set +e; "$python" "$stop_existing" "$vm_id" >/dev/null 2>&1 || true; wait_no_vm || true; install_file "$backup_virgl" "$app_virgl" "$old_virgl_hash" || true; install_file "$backup_crosvm" "$app_crosvm" "$old_crosvm_hash" || true; wait_stopped || true; "$python" "$start_existing" "$vm_id" >/dev/null 2>&1 || true; wait_running "$old_crosvm_hash" "$old_virgl_hash" || true; fi; exit "$status"' EXIT

"$python" "$stop_existing" "$vm_id"
wait_no_vm
wait_stopped
install_file "$stage/libvirglrenderer.so" "$app_virgl" "$new_virgl_hash"
install_file "$stage/crosvm" "$app_crosvm" "$new_crosvm_hash"
"$python" "$start_existing" "$vm_id"
wait_running "$new_crosvm_hash" "$new_virgl_hash"
rollback=0
trap - EXIT
echo "ACTIVE_CROSVM_SHA256=$new_crosvm_hash"
echo "ACTIVE_VIRGL_SHA256=$new_virgl_hash"
echo "DEPLOYED=1"
