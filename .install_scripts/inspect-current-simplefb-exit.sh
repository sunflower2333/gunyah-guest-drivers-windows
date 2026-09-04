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
cmd=$(tr '\000' ' ' <"/proc/$pid/cmdline")
vm_uuid=$(printf '%s\n' "$cmd" |
    sed -n 's/.*dvmin_\([0-9a-f-]*\)_sfb_mt\.sock.*/\1/p')
if [ -z "$vm_uuid" ]; then
    echo "failed to resolve VM UUID" >&2
    exit 4
fi

stderr_log="/data/data/cn.classfun.droidvm/cache/console_${vm_uuid}_stderr.log"
printf 'pid=%s\nexe=%s\n' "$pid" "$(readlink "/proc/$pid/exe")"
sha256sum "/proc/$pid/exe"
printf 'root_netns=%s\n' "$(readlink /proc/self/ns/net)"
printf 'crosvm_netns=%s\n' "$(readlink "/proc/$pid/ns/net")"
printf 'tcp_5900_rows:\n'
grep -i ':170C ' "/proc/$pid/net/tcp" "/proc/$pid/net/tcp6" 2>/dev/null || true

printf '\nfocused_display_log:\n'
stat -c 'size=%s mtime=%y path=%n' "$stderr_log"
grep -n -E 'VNC server|simplefb: failed to open display|simplefb display thread exited|simplefb: display connection closed|simplefb: dispatch_events error|simplefb: guest memory no longer readable|simplefb: gpu blit failed' \
    "$stderr_log" | tail -n 100 || true
