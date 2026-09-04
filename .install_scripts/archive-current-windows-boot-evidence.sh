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

cache_root=/data/data/cn.classfun.droidvm/cache
archive_root=/data/local/tmp/droidvm-windows-recovery-evidence/20260902-pre-restart-pid12888
if [ -e "$archive_root" ]; then
    echo "archive path already exists: $archive_root" >&2
    exit 5
fi

mkdir -p "$archive_root"
printf '%s\n' "$cmd" >"$archive_root/crosvm-cmdline.txt"
for suffix in serial1 stderr stdio; do
    source_log="$cache_root/console_${vm_uuid}_${suffix}.log"
    cp -p "$source_log" "$archive_root/"
done
cp -p "$cache_root/daemon.log" "$archive_root/"

(
    cd "$archive_root"
    sha256sum ./* >SHA256SUMS
)
sync

printf 'archive=%s\n' "$archive_root"
stat -c 'size=%s mtime=%y path=%n' "$archive_root"/*
printf '\nSHA256SUMS:\n'
cat "$archive_root/SHA256SUMS"
