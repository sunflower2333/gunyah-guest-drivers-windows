#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
stage=/data/data/com.termux/files/home/exclusive-bar-d9645060/kmt-host-snapshot
console_prefix=$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493

pid=$(pidof crosvm 2>/dev/null || true)
printf 'crosvm_pid=%s\n' "$pid"
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
tr '\000' ' ' <"/proc/$pid/cmdline"
printf '\n'

mkdir -p "$stage"
for suffix in stderr stdio uart; do
    source_file=${console_prefix}_${suffix}.log
    if [ -f "$source_file" ]; then
        cp -p "$source_file" "$stage/$suffix.log"
    fi
done
dmesg >"$stage/dmesg.txt"
sha256sum "$app_root/usr/bin/crosvm" >"$stage/crosvm.sha256"
sha256sum "$app_root/usr/lib/libvirglrenderer.so" >"$stage/libvirglrenderer.sha256"
find "$stage" -maxdepth 1 -type f -printf '%s %p\n' | sort
