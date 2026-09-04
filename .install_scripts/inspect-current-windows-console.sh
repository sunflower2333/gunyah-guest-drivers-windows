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
cache_root=/data/data/cn.classfun.droidvm/cache
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

printf 'pid=%s\nvm_uuid=%s\n' "$pid" "$vm_uuid"
printf '\n===== log identities =====\n'
for log in "$serial_log" "$stderr_log" "$stdio_log" "$daemon_log"; do
    printf 'path=%s\n' "$log"
    if [ -f "$log" ]; then
        stat -c '  mode=%A size=%s inode=%i mtime=%y' "$log"
        sha256sum "$log"
    else
        echo '  MISSING'
    fi
done

printf '\n===== complete serial1 =====\n'
if [ -f "$serial_log" ]; then
    sed -n '1,320p' "$serial_log"
fi

printf '\n===== stderr tail =====\n'
if [ -f "$stderr_log" ]; then
    tail -n 800 "$stderr_log"
fi

printf '\n===== stdio relationship =====\n'
if [ -f "$stderr_log" ] && [ -f "$stdio_log" ] && cmp -s "$stderr_log" "$stdio_log"; then
    echo 'stdio is byte-identical to stderr'
elif [ -f "$stdio_log" ]; then
    echo 'stdio differs from stderr; tail follows'
    tail -n 400 "$stdio_log"
fi

printf '\n===== focused daemon records =====\n'
if [ -f "$daemon_log" ]; then
    grep -n -E "$vm_uuid|crosvm|vm_status|start|stop|error|failed" "$daemon_log" |
        tail -n 500 || true
fi
