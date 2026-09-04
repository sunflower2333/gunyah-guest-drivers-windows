#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
console_prefix=$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493
stage=/data/data/com.termux/files/home/high-mmio-guard-8bffa853/post-kmt-evidence

pid=$(pidof crosvm 2>/dev/null || true)
printf 'crosvm_pid=%s\n' "$pid"
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1

rm -rf "$stage"
mkdir -p "$stage"
for suffix in stderr stdio uart; do
    source_file=${console_prefix}_$suffix.log
    if [ -f "$source_file" ]; then
        cp -p "$source_file" "$stage/$suffix.log"
    fi
done
dmesg >"$stage/dmesg.txt"
sha256sum "$app_root/usr/bin/crosvm" "$app_root/usr/lib/libvirglrenderer.so" >"$stage/runtime.sha256"
grep -Ei 'gunyah|gh_rm|gh_vm|memparcel|stage.?2|external abort|fault|virtio.?gpu|GUNYAH-HIGHMMIO|GUNYAH-ADD' \
    "$stage/stderr.log" "$stage/dmesg.txt" >"$stage/filtered.txt" || true
find "$stage" -maxdepth 1 -type f -printf '%s %p\n' | sort
