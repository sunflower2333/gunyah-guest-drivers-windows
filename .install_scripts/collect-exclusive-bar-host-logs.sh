#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
console_prefix=$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493
launcher_log=/data/local/tmp/crosvm-native-58069-exclusive-bar-d9645060.log
daemon_log=$app_root/cache/daemon.log

printf 'crosvm_pid=%s\n' "$(pidof crosvm 2>/dev/null || true)"

for log in \
    "$daemon_log" \
    "$launcher_log" \
    "${console_prefix}_stderr.log" \
    "${console_prefix}_stdio.log" \
    "${console_prefix}_uart.log"; do
    printf '\n===== %s =====\n' "$log"
    if [ -f "$log" ]; then
        stat -c 'size=%s mtime=%y' "$log"
        tail -n 400 "$log"
    else
        echo MISSING
    fi
done

printf '\n===== recent kernel and Gunyah messages =====\n'
dmesg | grep -Ei 'gunyah|gh_rm|gh_vm|memparcel|crosvm|external abort|stage.?2|virtio.?gpu' | tail -n 500 || true
