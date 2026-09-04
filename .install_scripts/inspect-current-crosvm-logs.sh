#!/system/bin/sh
set -eu
app_root=/data/data/cn.classfun.droidvm
pid=$(pidof crosvm 2>/dev/null || true)
printf 'crosvm_pid=%s\n' "$pid"
test -n "$pid"
cmd=$(tr '\000' ' ' </proc/$pid/cmdline)
printf 'cmd=%s\n' "$cmd"
name=$(printf '%s\n' "$cmd" | sed -n 's/.*--name \([^ ]*\).*/\1/p')
printf 'name=%s\n' "$name"
for log in "$app_root/cache/console_${name}_stderr.log" "$app_root/cache/console_${name}_stdio.log" "$app_root/cache/console_${name}_uart.log"; do
    printf '\n===== %s =====\n' "$log"
    if [ -f "$log" ]; then
        stat -c 'size=%s mtime=%y' "$log"
        tail -n 300 "$log"
    else
        echo MISSING
    fi
done
printf '\n===== filtered =====\n'
grep -Ei 'GPU-MAPBLOB|drm2kgsl|gunyah|gh_rm|gh_vm|memparcel|stage.?2|external abort|fault|virtio.?gpu' \
    "$app_root/cache/console_${name}_stderr.log" "$app_root/cache/console_${name}_stdio.log" 2>/dev/null | tail -n 500 || true
