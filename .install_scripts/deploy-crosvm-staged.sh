#!/system/bin/sh
set -eu

if [ "$#" -ne 8 ]; then
    echo "usage: $0 STAGED_CROSVM STAGED_CONFIG OLD_HASH NEW_HASH NAME CONFIG LOG ROLLBACK_CONFIG" >&2
    exit 2
fi

staged_crosvm=$1
staged_config=$2
old_hash=$3
new_hash=$4
expected_name=$5
new_config=$6
new_log=$7
rollback_config=$8

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_renderer=$app_root/usr/lib/libvirglrenderer.so
droidvm=$app_root/bin/droidvm
backup_dir=/data/local/tmp/droidvm-sda17/back/host
expected_image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
renderer_hash=50bf1dc1a5e75ea45a56a0b13c1c7e7b10a205856c0de8dda19140471ef946f6

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

install_crosvm()
{
    source_file=$1
    expected_hash=$2
    temporary=$app_crosvm.deploy-new
    owner=$(stat -c '%u:%g' "$app_crosvm")
    mode=$(stat -c '%a' "$app_crosvm")
    context=$(ls -Zd "$app_crosvm" | awk '{print $1}')

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
    rm -f "$log"
    nohup "$droidvm" start "$(cat "$config")" >"$log" 2>&1 </dev/null &
    echo "launcher_pid=$!"
}

test -z "$(pidof crosvm 2>/dev/null || true)"
test -x "$app_crosvm"
test -x "$droidvm"
test -f "$app_renderer"
test -f "$staged_crosvm"
test -f "$staged_config"
test -f "$rollback_config"
test -f "$expected_image"
test "$(hash_file "$app_crosvm")" = "$old_hash"
test "$(hash_file "$app_renderer")" = "$renderer_hash"
test "$(hash_file "$staged_crosvm")" = "$new_hash"
grep -F "\"name\":\"$expected_name\"" "$staged_config" >/dev/null
grep -F '"memory_mb":2048' "$staged_config" >/dev/null
grep -F '"gpu_drm2kgsl_pool_mb":8' "$staged_config" >/dev/null
grep -F '"gpu_pci_bar_size":8388608' "$staged_config" >/dev/null
grep -F "$expected_image" "$staged_config" >/dev/null

mkdir -p "$backup_dir"
backup=$backup_dir/crosvm-$old_hash
if [ -f "$backup" ]; then
    test "$(hash_file "$backup")" = "$old_hash"
else
    cp -p "$app_crosvm" "$backup"
    sync
    test "$(hash_file "$backup")" = "$old_hash"
fi
cp "$staged_config" "$new_config"
chmod 600 "$new_config"
sync

install_crosvm "$staged_crosvm" "$new_hash"
start_config "$new_config" "$new_log"

attempt=0
pid=
while [ "$attempt" -lt 60 ]; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        if [ "$(echo "$pid" | wc -w)" -ne 1 ]; then
            echo "Multiple crosvm processes after launch: $pid" >&2
            exit 1
        fi
        cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline")
        case "$cmdline" in
            *"--name $expected_name"*"--mem 2048"*"$expected_image"*) ;;
            *)
                echo "Unexpected crosvm after launch: $cmdline" >&2
                exit 1
                ;;
        esac
        case "$cmdline" in
            *"--pre-alloc drm-host-mb=8"*"pci-bar-size=8388608"*) break ;;
            *)
                echo "Unexpected GPU memory configuration: $cmdline" >&2
                exit 1
                ;;
        esac
    fi
    attempt=$((attempt + 1))
    sleep 1
done

stable=0
while [ -n "$pid" ] && [ "$stable" -lt 20 ]; do
    current=$(pidof crosvm 2>/dev/null || true)
    if [ "$current" != "$pid" ]; then
        pid=
        break
    fi
    stable=$((stable + 1))
    sleep 1
done

if [ -n "$pid" ] && [ "$stable" -eq 20 ]; then
    echo "crosvm_pid=$pid"
    echo "crosvm_hash=$(hash_file "$app_crosvm")"
    echo "renderer_hash=$(hash_file "$app_renderer")"
    echo "config=$new_config"
    echo "log=$new_log"
    echo "$cmdline"
    exit 0
fi

test -z "$(pidof crosvm 2>/dev/null || true)"
install_crosvm "$backup" "$old_hash"
start_config "$rollback_config" "$new_log.rollback"
echo "New crosvm failed to remain alive; previous crosvm restored and rollback launch requested" >&2
exit 1
