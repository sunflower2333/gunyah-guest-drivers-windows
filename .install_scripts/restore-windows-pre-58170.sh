#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    echo "root execution required" >&2
    exit 2
fi

backup=/mnt/pass_through/0/emulated/0/back/win11-droidvm-final-comp.pre-58170.qcow2
active=/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2
expected_backup_size=12526419968
expected_active_size=17701011456
expected_hash=1e17e86bd15386ab97e50b03f1678916f3dc27945a9e62f45fa1aec6da4d64e6
safety_kib=1048576

set -- $(pidof crosvm 2>/dev/null || true)
if [ "$#" -ne 0 ]; then
    echo "refusing recovery while crosvm is running: $*" >&2
    exit 3
fi

for path in "$backup" "$active"; do
    if [ ! -f "$path" ] || [ -L "$path" ]; then
        echo "expected a regular non-symlink file: $path" >&2
        exit 4
    fi
done

backup_size=$(stat -c %s "$backup")
active_size=$(stat -c %s "$active")
if [ "$backup_size" -ne "$expected_backup_size" ]; then
    echo "unexpected backup size: $backup_size" >&2
    exit 5
fi
if [ "$active_size" -ne "$expected_active_size" ]; then
    echo "unexpected pre-recovery active-image size: $active_size" >&2
    exit 6
fi

backup_device=$(stat -c %d "$backup")
active_device=$(stat -c %d "$active")
if [ "$backup_device" -ne "$active_device" ]; then
    echo "backup and active image are not on the same filesystem" >&2
    exit 7
fi

available_kib=$(df -k "$active" | awk 'NR == 2 { print $4 }')
active_allocated_kib=$(( $(stat -c %b "$active") / 2 ))
backup_allocated_kib=$(( $(stat -c %b "$backup") / 2 ))
recoverable_kib=$((available_kib + active_allocated_kib))
required_kib=$((backup_allocated_kib + safety_kib))
if [ "$recoverable_kib" -lt "$required_kib" ]; then
    echo "insufficient recoverable space: $recoverable_kib KiB < $required_kib KiB" >&2
    exit 8
fi

backup_hash=$(sha256sum "$backup" | awk '{ print $1 }')
if [ "$backup_hash" != "$expected_hash" ]; then
    echo "backup hash mismatch: $backup_hash" >&2
    exit 9
fi

printf 'source=%s\ndestination=%s\nbackup_size=%s\nbackup_hash=%s\n' \
    "$backup" "$active" "$backup_size" "$backup_hash"
printf 'available_kib=%s\nactive_allocated_kib=%s\nbackup_allocated_kib=%s\nsafety_kib=%s\n' \
    "$available_kib" "$active_allocated_kib" "$backup_allocated_kib" "$safety_kib"

cp -f "$backup" "$active"
sync

restored_size=$(stat -c %s "$active")
restored_hash=$(sha256sum "$active" | awk '{ print $1 }')
if [ "$restored_size" -ne "$expected_backup_size" ]; then
    echo "restored image size mismatch: $restored_size" >&2
    exit 10
fi
if [ "$restored_hash" != "$expected_hash" ]; then
    echo "restored image hash mismatch: $restored_hash" >&2
    exit 11
fi

source_hash_after=$(sha256sum "$backup" | awk '{ print $1 }')
if [ "$source_hash_after" != "$expected_hash" ]; then
    echo "backup source changed during recovery: $source_hash_after" >&2
    exit 12
fi

stat -c 'restored device=%d inode=%i size=%s blocks=%b mtime=%y path=%n' "$active"
df -k "$active"
printf 'RESTORE_PASS hash=%s\n' "$restored_hash"
