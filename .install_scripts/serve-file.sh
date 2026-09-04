#!/system/bin/sh
# Serve one file over the LAN so the Windows guest can pull it directly,
# bypassing the SSH tunnel that truncates sustained transfers.
set -eu
socat=/data/data/com.termux/files/usr/bin/socat
file=${1:-/data/local/tmp/zink-fixed.dll}
port=${2:-9000}
test -x "$socat"
test -f "$file"
pkill -f "TCP-LISTEN:$port" 2>/dev/null || true
nohup "$socat" "TCP-LISTEN:$port,bind=0.0.0.0,reuseaddr,fork" "OPEN:$file" \
    </dev/null >/data/local/tmp/socat-$port.log 2>&1 &
sleep 1
kill -0 $! 2>/dev/null && echo "SERVING $file on :$port pid=$!"
ss -ltn 2>/dev/null | grep ":$port" || true
