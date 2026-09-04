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
cache_root="$app_root/cache"
cmd=$(tr '\000' ' ' <"/proc/$pid/cmdline")
vm_uuid=$(printf '%s\n' "$cmd" |
    sed -n 's/.*dvmin_\([0-9a-f-]*\)_sfb_mt\.sock.*/\1/p')
if [ -z "$vm_uuid" ]; then
    echo "failed to resolve VM UUID from crosvm command line" >&2
    exit 4
fi

stderr_log="$cache_root/console_${vm_uuid}_stderr.log"
stdio_log="$cache_root/console_${vm_uuid}_stdio.log"
serial_log="$cache_root/console_${vm_uuid}_serial1.log"
daemon_log="$cache_root/daemon.log"

printf 'pid=%s\nvm_uuid=%s\ncmd=%s\n' "$pid" "$vm_uuid" "$cmd"

printf '\n===== exact log identities =====\n'
for log in "$serial_log" "$stderr_log" "$stdio_log" "$daemon_log"; do
    printf 'path=%s\n' "$log"
    if [ -f "$log" ]; then
        stat -c '  size=%s inode=%i mtime=%y' "$log"
        sha256sum "$log"
    else
        echo '  MISSING'
    fi
done

printf '\n===== serial tail (40) =====\n'
if [ -f "$serial_log" ]; then
    tail -n 40 "$serial_log"
fi

printf '\n===== crosvm final non-BAR tail (80) =====\n'
if [ -f "$stderr_log" ]; then
    tail -n 240 "$stderr_log" | grep -v 'GPU-BAR-CONFIG' | tail -n 80 || true
fi

printf '\n===== crosvm focused records (80) =====\n'
if [ -f "$stderr_log" ]; then
    grep -Ei 'qcow|block.*(error|fail)|disk.*(error|fail)|I/O error|input/output|fatal|panic|gpu reset|readback is black|vnc|rfb|netkvm|tap.*(error|fail)|socat' \
        "$stderr_log" | tail -n 80 || true
fi

printf '\n===== stdio relationship =====\n'
if [ -f "$stderr_log" ] && [ -f "$stdio_log" ] && cmp -s "$stderr_log" "$stdio_log"; then
    echo 'stdio is byte-identical to stderr'
elif [ -f "$stdio_log" ]; then
    echo 'stdio differs from stderr; final 80 lines follow'
    tail -n 80 "$stdio_log"
fi

printf '\n===== daemon final records (40) =====\n'
if [ -f "$daemon_log" ]; then
    tail -n 120 "$daemon_log" |
        grep -n -E "$vm_uuid|crosvm|vm_status|start|stop|error|failed" |
        tail -n 40 || true
fi

printf '\n===== host network =====\n'
ip -brief link show vma5b6863f-0 2>/dev/null || true
ip neigh show 2>/dev/null | grep -E '192\.168\.43\.161|169\.254\.15\.245|fe80::bdf3' || true
ss -lntp 2>/dev/null | grep -E ':(22|2222|5900)[[:space:]]' || true
