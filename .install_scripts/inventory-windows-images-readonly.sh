#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    echo "root execution required" >&2
    exit 2
fi

active_image=/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2

set -- $(pidof crosvm 2>/dev/null || true)
if [ "$#" -ne 1 ]; then
    echo "expected exactly one crosvm process, found $#" >&2
    exit 3
fi

pid=$1
cmd=$(tr '\000' ' ' <"/proc/$pid/cmdline")
case "$cmd" in
    *"--mem 2048"*"--block $active_image,lock=false"*) ;;
    *)
        echo "live crosvm does not match the expected 2048 MiB active image" >&2
        exit 4
        ;;
esac

printf 'pid=%s\nactive_image=%s\n' "$pid" "$active_image"
stat -c 'active device=%d inode=%i size=%s blocks=%b mtime=%y path=%n' "$active_image"

printf '\n===== filesystem and mount evidence =====\n'
df -k "$active_image"
mount | grep -E 'sda17|pass_through|emulated/0' || true

printf '\n===== candidate directory identities =====\n'
for candidate_dir in \
    /data/local/tmp/droidvm-sda17/back \
    /data/local/tmp/droidvm-sda17/bacp \
    /mnt/pass_through/0/emulated/0/back \
    /mnt/pass_through/0/emulated/0/bacp \
    /storage/emulated/0/back \
    /storage/emulated/0/bacp; do
    if [ -e "$candidate_dir" ]; then
        stat -c 'dir device=%d inode=%i mode=%A path=%n' "$candidate_dir"
    fi
done

printf '\n===== backup QCOW2 metadata =====\n'
found=0
for candidate_dir in \
    /data/local/tmp/droidvm-sda17/back \
    /data/local/tmp/droidvm-sda17/bacp \
    /mnt/pass_through/0/emulated/0/back \
    /mnt/pass_through/0/emulated/0/bacp \
    /storage/emulated/0/back \
    /storage/emulated/0/bacp; do
    if [ ! -d "$candidate_dir" ]; then
        continue
    fi
    find "$candidate_dir" -maxdepth 3 -type f -iname '*.qcow2' 2>/dev/null |
        while IFS= read -r candidate_image; do
            stat -c 'backup device=%d inode=%i size=%s blocks=%b mtime=%y path=%n' \
                "$candidate_image"
        done
    found=1
done

if [ "$found" -eq 0 ]; then
    echo 'no candidate back/bacp directory exists'
fi
