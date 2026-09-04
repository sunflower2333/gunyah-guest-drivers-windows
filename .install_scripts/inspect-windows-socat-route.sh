#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    exec su -c "/system/bin/sh $0"
fi

termux=/data/data/com.termux/files/usr/bin
tap=vma5b6863f-0

set -- $(pidof crosvm 2>/dev/null || true)
if [ "$#" -ne 1 ]; then
    echo "expected exactly one crosvm process, found $#" >&2
    exit 2
fi
echo "CROSVM_PID=$1"

echo '===== SOCAT_COMMANDS ====='
set -- $(pidof socat 2>/dev/null || true)
echo "SOCAT_COUNT=$#"
for pid in "$@"; do
    printf 'SOCAT_PID=%s CMD=' "$pid"
    xargs -0 < "/proc/$pid/cmdline"
done

echo '===== PORT_22_LISTENERS ====='
"$termux/ss" -ltnp | awk 'NR == 1 || $4 ~ /:22$/'

echo '===== TAP_ADDRESSES ====='
"$termux/ip" -br address show dev "$tap"

echo '===== TAP_NEIGHBORS ====='
"$termux/ip" neighbor show dev "$tap"

echo '===== ARP ====='
cat /proc/net/arp
