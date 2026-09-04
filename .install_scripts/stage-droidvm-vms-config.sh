#!/system/bin/sh
set -eu

source_path=/data/data/cn.classfun.droidvm/files/vms.json
stage_path=/data/data/com.termux/files/home/vms.pre-shim-parcel-20260830-005124.json

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root" >&2
    exit 1
fi

if [ ! -f "$source_path" ]; then
    echo "missing source config: $source_path" >&2
    exit 1
fi

if [ -e "$stage_path" ]; then
    echo "refusing to overwrite staging file: $stage_path" >&2
    exit 1
fi

cp --preserve=timestamps "$source_path" "$stage_path"
chown 10316:10316 "$stage_path"
chmod 600 "$stage_path"
cmp "$source_path" "$stage_path"
sha256sum "$stage_path"
ls -lZ "$stage_path"
