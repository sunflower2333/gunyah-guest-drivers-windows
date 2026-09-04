#!/system/bin/sh
set -eu

source_path=/data/data/cn.classfun.droidvm/files/vms.json
backup_path=/data/data/cn.classfun.droidvm/files/vms.json.pre-shim-parcel-20260830-005124

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root" >&2
    exit 1
fi

if [ ! -f "$source_path" ]; then
    echo "missing source config: $source_path" >&2
    exit 1
fi

if [ -e "$backup_path" ]; then
    echo "refusing to overwrite backup: $backup_path" >&2
    exit 1
fi

cp --preserve=all "$source_path" "$backup_path"
cmp "$source_path" "$backup_path"

source_hash=$(sha256sum "$source_path" | cut -d ' ' -f 1)
backup_hash=$(sha256sum "$backup_path" | cut -d ' ' -f 1)
if [ "$source_hash" != "$backup_hash" ]; then
    echo "backup hash mismatch" >&2
    exit 1
fi

echo "source_sha256=$source_hash"
echo "backup_sha256=$backup_hash"
stat -c 'source=%u:%g:%a:%s' "$source_path"
stat -c 'backup=%u:%g:%a:%s' "$backup_path"
ls -lZ "$source_path" "$backup_path"
