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
if [ ! -d "$archive_root" ]; then
    echo "missing archive: $archive_root" >&2
    exit 5
fi

printf 'current_pid=%s\nvm_uuid=%s\narchive=%s\n' "$pid" "$vm_uuid" "$archive_root"
printf '\n===== verified archive manifest =====\n'
cat "$archive_root/SHA256SUMS"
(
    cd "$archive_root"
    sha256sum -c SHA256SUMS
)

printf '\n===== archived versus current logs =====\n'
for suffix in serial1 stderr stdio; do
    name="console_${vm_uuid}_${suffix}.log"
    archived="$archive_root/$name"
    current="$cache_root/$name"
    printf 'name=%s\n' "$name"
    stat -c '  archived size=%s mtime=%y' "$archived"
    sha256sum "$archived"
    stat -c '  current  size=%s mtime=%y' "$current"
    sha256sum "$current"
    if cmp -s "$archived" "$current"; then
        echo '  relation=identical'
    else
        echo '  relation=different'
    fi
done

patterns='Boot0002|no exporter opened|stub display|simplefb: feeding|readback is black|gpu reset|drm_renderer_capset|qcow|block.*(error|fail)|I/O error|fatal|panic'
for label in archived current; do
    if [ "$label" = archived ]; then
        stderr_log="$archive_root/console_${vm_uuid}_stderr.log"
    else
        stderr_log="$cache_root/console_${vm_uuid}_stderr.log"
    fi
    printf '\n===== %s focused sequence =====\n' "$label"
    grep -Ein "$patterns" "$stderr_log" | tail -n 100 || true
done
