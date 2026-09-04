#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
config=/data/local/tmp/droidvm-native-working-58068-retry-458ffb2c.json
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
socket=$app_root/run/s-native-58068-shm-retry-458ffb2c.sock

printf 'crosvm_pid=%s\n' "$(pidof crosvm 2>/dev/null || true)"
printf 'crosvm_hash='
sha256sum "$app_root/usr/bin/crosvm"
printf 'renderer_hash='
sha256sum "$app_root/usr/lib/libvirglrenderer.so"

for path in "$config" "$image" "$socket"; do
    printf '\n===== %s =====\n' "$path"
    if [ -e "$path" ]; then
        stat -c 'type=%F size=%s mode=%a owner=%u:%g mtime=%y' "$path"
    else
        echo MISSING
    fi
done

printf '\n===== config =====\n'
cat "$config"

printf '\n===== cache files =====\n'
find "$app_root/cache" -maxdepth 1 -type f -printf '%TY-%Tm-%Td %TH:%TM:%TS %s %p\n' 2>/dev/null |
    sort | tail -n 120 || true

printf '\n===== recent launcher logs =====\n'
find /data/local/tmp -maxdepth 1 -type f -name 'crosvm-*.log' \
    -printf '%TY-%Tm-%Td %TH:%TM:%TS %s %p\n' 2>/dev/null | sort | tail -n 80 || true

printf '\n===== gunyah device nodes =====\n'
find /dev -maxdepth 2 \( -iname '*gunyah*' -o -iname 'gh_*' \) -ls 2>/dev/null || true
