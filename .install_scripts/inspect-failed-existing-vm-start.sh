#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    echo "root execution required" >&2
    exit 2
fi

vm_id=a5b6863f-6bbf-4af4-97f3-2a4653f6edb7
app_root=/data/data/cn.classfun.droidvm
cache_root=$app_root/cache
app_crosvm=$app_root/usr/bin/crosvm
app_virgl=$app_root/usr/lib/libvirglrenderer.so
image=/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2

printf 'crosvm_pids=%s\n' "$(pidof crosvm 2>/dev/null || true)"

printf '\n===== active inputs =====\n'
for file in "$app_crosvm" "$app_virgl" "$image"; do
    if [ -f "$file" ]; then
        stat -c 'size=%s mtime=%y path=%n' "$file"
        case "$file" in
            "$app_crosvm"|"$app_virgl") sha256sum "$file" ;;
        esac
    else
        printf 'MISSING path=%s\n' "$file"
    fi
done

printf '\n===== exact VM logs =====\n'
for suffix in stderr stdio serial1; do
    log=$cache_root/console_${vm_id}_${suffix}.log
    printf '\n--- %s ---\n' "$log"
    if [ -f "$log" ]; then
        stat -c 'size=%s mtime=%y' "$log"
        tail -n 180 "$log"
    else
        echo MISSING
    fi
done

printf '\n===== daemon records =====\n'
daemon_log=$cache_root/daemon.log
if [ -f "$daemon_log" ]; then
    stat -c 'size=%s mtime=%y' "$daemon_log"
    tail -n 500 "$daemon_log" |
        grep -n -E "$vm_id|crosvm|vm_start|vm_stop|running|stopped|error|failed|exit" |
        tail -n 160 || true
else
    echo MISSING
fi

printf '\n===== bounded logcat =====\n'
logcat -d -v threadtime 2>/dev/null |
    tail -n 1200 |
    grep -Ei "$vm_id|crosvm|DroidVM" |
    tail -n 200 || true
