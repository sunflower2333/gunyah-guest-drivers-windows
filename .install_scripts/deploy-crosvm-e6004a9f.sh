#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
droidvm=$app_root/bin/droidvm
library_path=$app_root/usr/lib:$app_root/lib
stage=/data/data/com.termux/files/home/crosvm-e6004a9f
backup_dir=/data/local/tmp/droidvm-sda17/back/host
backup=$backup_dir/crosvm-3fc4b5bb97ff2eb6
source_config=/data/local/tmp/droidvm-native-working-58052-2fb28449.json
current_socket=$app_root/run/s-native-58052-r2fb28449.sock
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
old_hash=3fc4b5bb97ff2eb656f32ea8d5b2db3902ca24bd28ec0ca8872df9f32248752e
new_hash=e6004a9f8b31b327dd87b9ab9e5d68a83637392c94cc8e3db17b9d96bcb9d789

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

install_binary()
{
    source_file=$1
    expected_hash=$2
    owner=$3
    mode=$4
    context=$5
    temporary=$app_crosvm.deploy-new

    rm -f "$temporary"
    cp "$source_file" "$temporary"
    chown "$owner" "$temporary"
    chmod "$mode" "$temporary"
    chcon "$context" "$temporary"
    test "$(hash_file "$temporary")" = "$expected_hash"
    mv -f "$temporary" "$app_crosvm"
    sync
    test "$(hash_file "$app_crosvm")" = "$expected_hash"
}

start_config()
{
    config=$1
    log=$2
    nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
    echo "launcher_pid=$!"
}

test -x "$app_crosvm"
test -x "$droidvm"
test -f "$stage"
test -f "$source_config"
test -f "$expected_image"
test "$(hash_file "$app_crosvm")" = "$old_hash"
test "$(hash_file "$stage")" = "$new_hash"
grep -F '"memory_mb":2048' "$source_config" >/dev/null
grep -F '"gpu_udmabuf":true' "$source_config" >/dev/null
grep -F '"gpu_mode":"native"' "$source_config" >/dev/null
grep -F '"gpu_provider":"drm2kgsl"' "$source_config" >/dev/null
grep -F '"display_backend":"simplefb"' "$source_config" >/dev/null
grep -F "$expected_image" "$source_config" >/dev/null

pid="$(pidof crosvm 2>/dev/null || true)"
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
cmdline="$(tr '\000' ' ' <"/proc/$pid/cmdline")"
case "$cmdline" in
    *"--name s-native-58052-r2fb28449"*"--mem 2048"*"$expected_image"*) ;;
    *)
        echo "Refusing to stop unexpected crosvm: $cmdline" >&2
        exit 1
        ;;
esac

owner="$(stat -c '%u:%g' "$app_crosvm")"
mode="$(stat -c '%a' "$app_crosvm")"
context="$(ls -Zd "$app_crosvm" | awk '{print $1}')"
mkdir -p "$backup_dir"
if [ -f "$backup" ]; then
    test "$(hash_file "$backup")" = "$old_hash"
else
    cp -p "$app_crosvm" "$backup"
    sync
    test "$(hash_file "$backup")" = "$old_hash"
fi

LD_LIBRARY_PATH="$library_path" "$app_crosvm" stop "$current_socket"
attempt=0
while pidof crosvm >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 45 ]; then
        echo "crosvm did not stop within 45 seconds" >&2
        exit 1
    fi
    sleep 1
done

install_binary "$stage" "$new_hash" "$owner" "$mode" "$context"

new_id="$(cat /proc/sys/kernel/random/uuid)"
short_id="$(echo "$new_id" | cut -c1-8)"
new_name="s-native-58062-h$short_id"
new_config=/data/local/tmp/droidvm-native-working-58062-$short_id.json
new_log=/data/local/tmp/crosvm-native-58062-$short_id.log
sed -e "s|\"id\":\"2fb28449-b576-4786-81dc-3916a56d24f2\"|\"id\":\"$new_id\"|g" \
    -e "s|\"name\":\"s-native-58052-r2fb28449\"|\"name\":\"$new_name\"|g" \
    "$source_config" >"$new_config"
grep -F "\"id\":\"$new_id\"" "$new_config" >/dev/null
grep -F "\"name\":\"$new_name\"" "$new_config" >/dev/null
grep -F '"memory_mb":2048' "$new_config" >/dev/null
grep -F "$expected_image" "$new_config" >/dev/null

start_config "$new_config" "$new_log"
attempt=0
while :; do
    pid="$(pidof crosvm 2>/dev/null || true)"
    if [ -n "$pid" ]; then
        cmdline="$(tr '\000' ' ' <"/proc/$pid/cmdline")"
        case "$cmdline" in
            *"--name $new_name"*"--mem 2048"*"$expected_image"*)
                echo "crosvm_pid=$pid"
                echo "crosvm_hash=$(hash_file "$app_crosvm")"
                echo "config=$new_config"
                echo "vm_id=$new_id"
                echo "vm_name=$new_name"
                echo "$cmdline"
                exit 0
                ;;
            *)
                echo "Unexpected crosvm after deployment: $cmdline" >&2
                break
                ;;
        esac
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 60 ]; then
        echo "New crosvm did not start within 60 seconds; restoring the previous binary" >&2
        break
    fi
    sleep 1
done

pid="$(pidof crosvm 2>/dev/null || true)"
if [ -n "$pid" ]; then
    echo "Refusing automatic rollback while crosvm PID $pid is still live" >&2
    exit 1
fi
install_binary "$backup" "$old_hash" "$owner" "$mode" "$context"
rollback_id="$(cat /proc/sys/kernel/random/uuid)"
rollback_short="$(echo "$rollback_id" | cut -c1-8)"
rollback_name="s-native-58052-rollback-$rollback_short"
rollback_config=/data/local/tmp/droidvm-native-working-58052-rollback-$rollback_short.json
sed -e "s|\"id\":\"2fb28449-b576-4786-81dc-3916a56d24f2\"|\"id\":\"$rollback_id\"|g" \
    -e "s|\"name\":\"s-native-58052-r2fb28449\"|\"name\":\"$rollback_name\"|g" \
    "$source_config" >"$rollback_config"
start_config "$rollback_config" /data/local/tmp/crosvm-native-58052-rollback-$rollback_short.log
echo "Previous crosvm restored; rollback VM launch requested with $rollback_config" >&2
exit 1
