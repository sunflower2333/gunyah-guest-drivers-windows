#!/system/bin/sh
set -eu

src=/data/data/com.termux/files/home/vms-58030-output.json
dst=/data/data/cn.classfun.droidvm/files/vms.json
new=${dst}.new-58030
backup=/data/local/tmp/droidvm-sda17/back/vms.json.pre-58030-display-20260824-1215

if [ -n "$(pidof crosvm || true)" ]; then
    echo "refusing to update vms.json while crosvm is running" >&2
    exit 1
fi

test -s "$src"
test -s "$dst"
test -s "$backup"

owner_uid=$(stat -c %u "$dst")
owner_gid=$(stat -c %g "$dst")
mode=$(stat -c %a "$dst")

am force-stop cn.classfun.droidvm
cp "$src" "$new"
chown "$owner_uid:$owner_gid" "$new"
chmod "$mode" "$new"
restorecon "$new"
mv -f "$new" "$dst"
restorecon "$dst"
sync

cmp "$src" "$dst"
stat -c '%n uid=%u gid=%g mode=%a size=%s' "$dst"
grep -F 'win11-droidvm-final-comp.work-58030.qcow2' "$dst"
