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
echo "EXE=$(readlink /proc/$pid/exe)"
sha256sum "/proc/$pid/exe"

echo "CMDLINE_BEGIN"
xargs -0 -n 1 < "/proc/$pid/cmdline"
echo "CMDLINE_END"

echo "MAPS_BEGIN"
awk '$6 ~ /(crosvm|virgl|rutabaga)/ { print }' "/proc/$pid/maps"
echo "MAPS_END"

echo "MAPPED_HASHES_BEGIN"
awk '$6 ~ /(crosvm|virgl|rutabaga)/ { print $6 }' "/proc/$pid/maps" | sort -u |
while IFS= read -r path; do
    if [ -f "$path" ]; then
        sha256sum "$path"
    else
        echo "UNREADABLE $path"
    fi
done
echo "MAPPED_HASHES_END"
