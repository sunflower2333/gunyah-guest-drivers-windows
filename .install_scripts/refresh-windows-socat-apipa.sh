#!/system/bin/sh
set -eu

guest_ip=169.254.15.245
old_guest_ip=192.168.43.161
bridge=brd75f2fb9
route_table=200
socat=/data/data/com.termux/files/usr/bin/socat
log=/data/local/tmp/windows-socat-22.log
route_added=0
old_removed=0
new_pid=

start_forward() {
    target=$1
    nohup "$socat" \
        TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr \
        "TCP4:$target:22" >"$log" 2>&1 &
    forward_pid=$!
    sleep 1
    kill -0 "$forward_pid"
    printf '%s\n' "$forward_pid"
}

rollback() {
    status=$?
    trap - EXIT
    if [ "$status" -ne 0 ]; then
        if [ -n "$new_pid" ]; then
            kill "$new_pid" 2>/dev/null || true
        fi
        if [ "$old_removed" -eq 1 ]; then
            start_forward "$old_guest_ip" >/dev/null 2>&1 || true
        fi
        if [ "$route_added" -eq 1 ]; then
            ip route del "$guest_ip/32" dev "$bridge" table "$route_table" 2>/dev/null || true
        fi
    fi
    exit "$status"
}
trap rollback EXIT

test "$(id -u)" = 0
test -x "$socat"
ip link show dev "$bridge" | grep -q 'state UP'

if ! ip route show table "$route_table" | grep -Fq "$guest_ip dev $bridge"; then
    ip route add "$guest_ip/32" dev "$bridge" table "$route_table"
    route_added=1
fi

banner=$(timeout 8 "$socat" - "TCP4:$guest_ip:22,connect-timeout=5" 2>/dev/null | head -n 1 || true)
case "$banner" in
    SSH-*) ;;
    *)
        echo "Windows SSH banner unavailable at $guest_ip:22" >&2
        exit 1
        ;;
esac

old_pid=
for pid in $(pidof socat 2>/dev/null || true); do
    cmd=$(xargs -0 <"/proc/$pid/cmdline")
    case "$cmd" in
        *"TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr TCP4:$guest_ip:22"*)
            echo "WINDOWS_GUEST_IP=$guest_ip"
            echo "SOCAT_PID=$pid"
            echo "SSH_BANNER=$banner"
            echo "ALREADY_CURRENT=1"
            trap - EXIT
            exit 0
            ;;
        *"TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr TCP4:$old_guest_ip:22"*)
            test -z "$old_pid"
            old_pid=$pid
            ;;
        *"TCP-LISTEN:22,"*)
            echo "Unexpected port-22 socat command for PID $pid: $cmd" >&2
            exit 1
            ;;
    esac
done

test -n "$old_pid"
kill "$old_pid"
old_removed=1
sleep 1

new_pid=$(start_forward "$guest_ip")
cmd=$(xargs -0 <"/proc/$new_pid/cmdline")
case "$cmd" in
    *"TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr TCP4:$guest_ip:22"*) ;;
    *) exit 1 ;;
esac

echo "WINDOWS_GUEST_IP=$guest_ip"
echo "SOCAT_PID=$new_pid"
echo "SSH_BANNER=$banner"
echo "UPDATED=1"
trap - EXIT
