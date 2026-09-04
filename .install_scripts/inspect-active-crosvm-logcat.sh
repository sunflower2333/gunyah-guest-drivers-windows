#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    exec su -c "/system/bin/sh $0"
fi

set -- $(pidof crosvm)
if [ "$#" -ne 1 ]; then
    echo "expected exactly one crosvm process, found $#" >&2
    exit 2
fi

pid=$1
echo "PID=$pid"
echo "FD1=$(readlink /proc/$pid/fd/1)"
echo "FD2=$(readlink /proc/$pid/fd/2)"
logcat -d --pid="$pid" -v threadtime |
    grep -E 'VA slice|context|drm2kgsl_renderer_create|GPU-CREATE-BLOB|out of VA|destroy' |
    tail -n 240 || true

cache_root=/data/data/cn.classfun.droidvm/cache
stderr_log=$(find "$cache_root" -maxdepth 1 -type f \
    -name 'console_*_stderr.log' -printf '%T@ %p\n' 2>/dev/null |
    sort -nr | head -n 1 | cut -d' ' -f2-)
echo "STDERR_LOG=$stderr_log"
if [ -n "$stderr_log" ]; then
    stat -c 'STDERR_SIZE=%s STDERR_MTIME=%y' "$stderr_log"
    grep -n -E 'VA slice|context|drm2kgsl_renderer_create|GPU-CREATE-BLOB|out of VA|destroy' \
        "$stderr_log" | tail -n 240 || true
fi
