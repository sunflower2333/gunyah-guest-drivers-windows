#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    echo "root execution required" >&2
    exit 2
fi

set -- $(pidof crosvm 2>/dev/null || true)
if [ "$#" -ne 1 ]; then
    echo "expected exactly one crosvm process, found $#" >&2
    exit 3
fi

pid=$1
app_root=/data/data/cn.classfun.droidvm
cmd=$(tr '\000' ' ' <"/proc/$pid/cmdline")
tap=$(printf '%s\n' "$cmd" | sed -n 's/.*tap-name=\([^, ]*\).*/\1/p')
image=$(printf '%s\n' "$cmd" | sed -n 's/.*--block \([^, ]*\).*/\1/p')
pflash=$(printf '%s\n' "$cmd" | sed -n 's/.*--pflash path=\([^, ]*\).*/\1/p')

printf '===== process =====\n'
printf 'pid=%s\ncmd=%s\n' "$pid" "$cmd"
ps -A -o PID,PPID,STAT,ELAPSED,NAME,ARGS 2>/dev/null |
    grep -E '(^|[[:space:]])(crosvm|droidvmd|socat)([[:space:]]|$)' || true

printf '\n===== live fds =====\n'
for fd in 0 1 2 97 136; do
    target=$(readlink "/proc/$pid/fd/$fd" 2>/dev/null || true)
    printf 'fd=%s target=%s\n' "$fd" "$target"
    if [ -e "/proc/$pid/fd/$fd" ]; then
        stat -L -c '  mode=%A size=%s inode=%i mtime=%y' "/proc/$pid/fd/$fd" || true
    fi
done
ls -l "/proc/$pid/fd" 2>/dev/null |
    grep -E 'qcow|edk2|pipe:|socket:|/dev/pts|/dev/tty|/mnt/' || true

printf '\n===== active storage metadata =====\n'
for item in "$image" "$pflash"; do
    printf 'path=%s\n' "$item"
    if [ -n "$item" ] && [ -f "$item" ]; then
        stat -c '  mode=%A size=%s inode=%i mtime=%y' "$item"
    else
        echo '  MISSING'
    fi
done

printf '\n===== recent DroidVM files =====\n'
find "$app_root/cache" "$app_root/run" -maxdepth 2 -type f \
    -printf '%T@ %s %p\n' 2>/dev/null |
    sort -nr | head -n 100 || true

printf '\n===== crosvm logcat =====\n'
logcat -d --pid="$pid" -v threadtime 2>/dev/null | tail -n 1200 || true

printf '\n===== filtered global logcat =====\n'
logcat -d -v threadtime 2>/dev/null |
    grep -Ei 'crosvm|droidvm|uefi|edk2|windows|boot|qcow|virtio|gunyah|panic|fatal|error' |
    tail -n 1200 || true

printf '\n===== filtered kernel log =====\n'
dmesg 2>/dev/null |
    grep -Ei 'crosvm|gunyah|gh_vm|virtio|iommu|fault|abort|panic|error' |
    tail -n 800 || true

printf '\n===== guest network path =====\n'
printf 'tap=%s\n' "$tap"
if [ -n "$tap" ]; then
    ip -details link show "$tap" 2>/dev/null || true
    ip address show dev "$tap" 2>/dev/null || true
fi
ip neighbor show 2>/dev/null || true
ss -lntp 2>/dev/null | grep -E ':(22|2222|5900)([[:space:]]|$)' || true
