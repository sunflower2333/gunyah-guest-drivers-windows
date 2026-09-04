#!/system/bin/sh
set -eu

tap=vma5b6863f-0
guest=192.168.43.161
termux=/data/data/com.termux/files/usr/bin

printf '===== processes =====\n'
printf 'crosvm='; pidof crosvm 2>/dev/null || true
printf 'socat='; pidof socat 2>/dev/null || true

printf '\n===== addresses =====\n'
"$termux/ip" -br address show

printf '\n===== routes =====\n'
"$termux/ip" route show table all

printf '\n===== rules =====\n'
"$termux/ip" rule show

printf '\n===== neighbors =====\n'
"$termux/ip" neighbor show

printf '\n===== arp =====\n'
cat /proc/net/arp

printf '\n===== listeners =====\n'
"$termux/ss" -ltnp

printf '\n===== tap counters =====\n'
"$termux/ip" -s link show "$tap"

printf '\n===== guest reachability =====\n'
ping -c 1 -W 1 "$guest" || true

printf '\n===== available diagnostics =====\n'
for tool in tcpdump nc ncat socat; do
    if [ -x "$termux/$tool" ]; then
        printf '%s=%s\n' "$tool" "$termux/$tool"
    else
        printf '%s=missing\n' "$tool"
    fi
done
