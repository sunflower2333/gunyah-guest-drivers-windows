#!/system/bin/sh
set -eu

# Re-point the host-side Windows SSH forwarder at the guest's CURRENT address.
#
# The Windows VM is bridged onto the host's wlan card, so it takes a DHCP
# lease from the same LAN as wlan0 and its address changes across leases.
# Nothing here is hardcoded: the tap name and guest MAC come from the live
# crosvm command line, the bridge comes from the tap's master, and the guest
# IPv4 comes from the neighbour table entry for that MAC.
#
# Read-only with respect to the VM, its image, EFI, firmware, and DroidVM
# configuration. It only replaces a userspace socat forwarder and, if needed,
# a /32 route in the DroidVM policy table.

termux=/data/data/com.termux/files/usr/bin
socat="$termux/socat"
route_table=200
log=/data/local/tmp/windows-socat-22.log
route_added=0
route_ip=
old_removed=0
old_target=
new_pid=

start_forward() {
    target=$1
    # Detach stdin as well as stdout/stderr: the forwarder outlives this
    # script, and an inherited stdin keeps a caller's SSH channel open.
    nohup "$socat" \
        TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr \
        "TCP4:$target:22" </dev/null >"$log" 2>&1 &
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
        if [ "$old_removed" -eq 1 ] && [ -n "$old_target" ]; then
            start_forward "$old_target" >/dev/null 2>&1 || true
        fi
        if [ "$route_added" -eq 1 ] && [ -n "$route_ip" ]; then
            ip route del "$route_ip/32" dev "$bridge" table "$route_table" 2>/dev/null || true
        fi
    fi
    exit "$status"
}
trap rollback EXIT

test "$(id -u)" = 0
test -x "$socat"

# 1. Exactly one crosvm, and its network parameters.
pids=$(pidof crosvm 2>/dev/null || true)
set -- $pids
test "$#" -eq 1 || { echo "Expected exactly one crosvm, found $#: $pids" >&2; exit 1; }
pid=$1
cmdline=$(tr '\0' ' ' <"/proc/$pid/cmdline")

mac=$(printf '%s' "$cmdline" | grep -o 'mac=[0-9a-fA-F][0-9a-fA-F:]*' | head -n 1 | cut -d= -f2 | tr 'A-F' 'a-f')
tap=$(printf '%s' "$cmdline" | grep -o 'tap-name=[^ ,]*' | head -n 1 | cut -d= -f2)
test -n "$mac"
test -n "$tap"

# 2. The bridge the tap is enslaved to.
bridge=$(ip link show dev "$tap" | head -n 1 | sed -n 's/.* master \([^ ]*\) .*/\1/p')
test -n "$bridge"
ip link show dev "$bridge" | grep -q 'state UP'

# 3. The guest IPv4, resolved from the neighbour table by MAC.
resolve_guest_ip() {
    ip neigh show dev "$bridge" \
        | grep -i "lladdr $mac" \
        | grep -o '^[0-9][0-9.]*' \
        | grep -v ':' \
        | head -n 1
}

guest_ip=$(resolve_guest_ip || true)

# 4. Fallback: Windows drops ICMP, so populate the neighbour table with a
#    bounded TCP-22 sweep of the bridge subnet, then resolve by MAC again.
if [ -z "$guest_ip" ]; then
    echo "No neighbour entry for $mac; sweeping the bridge subnet" >&2
    prefix=$(ip -4 addr show dev "$bridge" \
        | sed -n 's#.*inet \([0-9]*\.[0-9]*\.[0-9]*\)\.[0-9]*/.*#\1#p' | head -n 1)
    test -n "$prefix"
    host=1
    while [ "$host" -le 254 ]; do
        timeout 1 "$socat" -T1 /dev/null "TCP4:$prefix.$host:22,connect-timeout=1" \
            >/dev/null 2>&1 || true
        host=$((host + 1))
    done
    guest_ip=$(resolve_guest_ip || true)
fi

test -n "$guest_ip" || { echo "Could not resolve a guest IPv4 for $mac on $bridge" >&2; exit 1; }

# 5. Route for the discovered address in the DroidVM policy table.
if ! ip route show table "$route_table" | grep -Fq "$guest_ip dev $bridge"; then
    ip route add "$guest_ip/32" dev "$bridge" table "$route_table"
    route_added=1
    route_ip=$guest_ip
fi

# 6. The address must actually answer as Windows SSH before anything changes.
banner=$(timeout 8 "$socat" - "TCP4:$guest_ip:22,connect-timeout=5" 2>/dev/null | head -n 1 || true)
case "$banner" in
    SSH-*) ;;
    *) echo "Windows SSH banner unavailable at $guest_ip:22" >&2; exit 1 ;;
esac

# 7. Replace the forwarder only if its target is stale.
old_pid=
for candidate in $(pidof socat 2>/dev/null || true); do
    cmd=$(xargs -0 <"/proc/$candidate/cmdline")
    case "$cmd" in
        *"TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr TCP4:$guest_ip:22"*)
            echo "GUEST_MAC=$mac"
            echo "GUEST_TAP=$tap"
            echo "GUEST_BRIDGE=$bridge"
            echo "WINDOWS_GUEST_IP=$guest_ip"
            echo "SOCAT_PID=$candidate"
            echo "SSH_BANNER=$banner"
            echo "ALREADY_CURRENT=1"
            trap - EXIT
            exit 0
            ;;
        *"TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr TCP4:"*)
            test -z "$old_pid"
            old_pid=$candidate
            old_target=${cmd##*TCP4:}
            old_target=${old_target%:22}
            ;;
        *"TCP-LISTEN:22,"*)
            echo "Unexpected port-22 socat command for PID $candidate: $cmd" >&2
            exit 1
            ;;
    esac
done

if [ -n "$old_pid" ]; then
    kill "$old_pid"
    old_removed=1
    sleep 1
fi

new_pid=$(start_forward "$guest_ip")
cmd=$(xargs -0 <"/proc/$new_pid/cmdline")
case "$cmd" in
    *"TCP-LISTEN:22,bind=0.0.0.0,fork,reuseaddr TCP4:$guest_ip:22"*) ;;
    *) exit 1 ;;
esac

echo "GUEST_MAC=$mac"
echo "GUEST_TAP=$tap"
echo "GUEST_BRIDGE=$bridge"
echo "WINDOWS_GUEST_IP=$guest_ip"
echo "PREVIOUS_TARGET=${old_target:-none}"
echo "SOCAT_PID=$new_pid"
echo "SSH_BANNER=$banner"
echo "UPDATED=1"
trap - EXIT
