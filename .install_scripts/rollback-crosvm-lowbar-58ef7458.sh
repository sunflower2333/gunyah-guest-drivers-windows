#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
termux_python=/data/data/com.termux/files/usr/bin/python3
stage=/data/data/com.termux/files/home/crosvm-lowbar-58ef7458
start_existing=$stage/droidvm-start-existing.py
vm_id=f227cc7b-2a94-4a17-ab7d-54d4f2a1b5b3
vm_name=s-remote-test
backup=/data/local/tmp/droidvm-crosvm-backups/crosvm-4a9182c33e487372dd4189ca5556aaec49f48cdb8da35e7882fded88234e4a4f
old_hash=4a9182c33e487372dd4189ca5556aaec49f48cdb8da35e7882fded88234e4a4f
new_hash=58ef745806fd30341b2110eed8f07b54eabdeed61b9b62232e736f65cc548a8e
start_log=/data/local/tmp/droidvm-lowbar-rollback-start.log

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

test -z "$(pidof crosvm 2>/dev/null || true)"
test "$(hash_file "$app_crosvm")" = "$new_hash"
test "$(hash_file "$backup")" = "$old_hash"
test -x "$termux_python"
test -f "$start_existing"

temporary=$app_crosvm.rollback-old
owner=$(stat -c '%u:%g' "$app_crosvm")
mode=$(stat -c '%a' "$app_crosvm")
context=$(ls -Zd "$app_crosvm" | awk '{print $1}')
rm -f "$temporary"
cp "$backup" "$temporary"
chown "$owner" "$temporary"
chmod "$mode" "$temporary"
chcon "$context" "$temporary"
test "$(hash_file "$temporary")" = "$old_hash"
mv -f "$temporary" "$app_crosvm"
sync
test "$(hash_file "$app_crosvm")" = "$old_hash"

: >"$start_log"
"$termux_python" "$start_existing" "$vm_id" >>"$start_log" 2>&1

attempt=0
pid=
while [ "$attempt" -lt 90 ]; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
case "$cmdline" in
    *"--name $vm_name"*"--mem 2048"*"win11-droidvm-final-comp.qcow2"*) ;;
    *)
        echo "Unexpected crosvm after rollback: $cmdline" >&2
        exit 1
        ;;
esac

sleep 15
if [ "$(pidof crosvm 2>/dev/null || true)" != "$pid" ]; then
    echo "Rolled-back crosvm $pid exited during the stability window" >&2
    exit 1
fi

echo "crosvm_pid=$pid"
echo "crosvm_hash=$old_hash"
echo "$cmdline"
