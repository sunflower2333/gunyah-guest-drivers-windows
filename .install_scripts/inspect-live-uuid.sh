#!/system/bin/sh
set -eu
root=/data/data/cn.classfun.droidvm/cache
pid=$(pidof crosvm 2>/dev/null || true)
printf 'crosvm_pid=%s\n' "$pid"
test -n "$pid"
printf 'exe='; readlink "/proc/$pid/exe" || true
printf 'cmd='; tr '\000' ' ' <"/proc/$pid/cmdline"; printf '\n'
for suffix in stderr stdio serial1 uart; do
    file=$(find "$root" -maxdepth 1 -type f -name "console_*_${suffix}.log" -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | cut -d' ' -f2-)
    if [ -z "$file" ]; then
        printf 'missing=%s\n' "$suffix"
        continue
    fi
    printf '\n===== %s (%s) =====\n' "$suffix" "$file"
    stat -c 'size=%s mtime=%y' "$file" 2>/dev/null || true
    tail -n 800 "$file" | grep -Ei 'GPU-|drm2kgsl|gunyah|gh_rm|gh_vm|memparcel|stage.?2|fault|virtio.?gpu|BAR|native|CTX|RESOURCE_MAP|blob|arena|shmem' || true
done
printf '\n===== daemon markers =====\n'
tail -n 500 "$root/daemon.log" | grep -Ei 'VM s|crosvm|START|RUNNING|STOPPED|exited|reboot' || true
