#!/system/bin/sh
set -eu

if [ "$(id -u)" != "0" ]; then
    exec su -c "/system/bin/sh $0"
fi

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_virgl=$app_root/usr/lib/libvirglrenderer.so
stage=/data/data/com.termux/files/home/crosvm-context-diagnostics-5f60c6e6
staged_crosvm=$stage/crosvm
staged_virgl=$stage/libvirglrenderer.so
backup_dir=/data/local/tmp/droidvm-crosvm-backups/context-diagnostics-20260831
old_crosvm_hash=35552b033fcd7a4200013c21a0974956a75a3f3dcbfcc442220708ecd8637ba0
old_virgl_hash=18316a28baae637629c12319135313c5a33d0ce75995edb5a1087dc399218994
new_crosvm_hash=5f60c6e652fe9c2398f24e12424274f554520b763a98fcb295bae06a26aea03a
new_virgl_hash=f980efd60d71cc2c7717d826b63ccb7b0247563e8831f2cf51db289b1d7fad4c

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

require_fragment()
{
    case "$1" in
        *"$2"*) ;;
        *)
            echo "missing expected crosvm argument: $2" >&2
            exit 3
            ;;
    esac
}

backup_file()
{
    source_file=$1
    expected_hash=$2
    backup_file=$3

    if [ -f "$backup_file" ]; then
        test "$(hash_file "$backup_file")" = "$expected_hash"
        return
    fi

    cp -p "$source_file" "$backup_file"
    sync
    test "$(hash_file "$backup_file")" = "$expected_hash"
}

set -- $(pidof crosvm)
if [ "$#" -ne 1 ]; then
    echo "expected exactly one crosvm process, found $#" >&2
    exit 2
fi

pid=$1
test "$(readlink /proc/$pid/exe)" = "$app_crosvm"
test "$(hash_file "$app_crosvm")" = "$old_crosvm_hash"
test "$(hash_file "$app_virgl")" = "$old_virgl_hash"
test "$(hash_file "$staged_crosvm")" = "$new_crosvm_hash"
test "$(hash_file "$staged_virgl")" = "$new_virgl_hash"

cmdline=$(xargs -0 < "/proc/$pid/cmdline")
require_fragment "$cmdline" "--name s"
require_fragment "$cmdline" "--mem 2048"
require_fragment "$cmdline" "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2"
require_fragment "$cmdline" "context-types=drm"
require_fragment "$cmdline" "udmabuf=true"
require_fragment "$cmdline" "fixed-blob-mapping=true"
require_fragment "$cmdline" "pci-bar-size=8388608"

mkdir -p "$backup_dir"
backup_crosvm=$backup_dir/crosvm-$old_crosvm_hash
backup_virgl=$backup_dir/libvirglrenderer.so-$old_virgl_hash
backup_file "$app_crosvm" "$old_crosvm_hash" "$backup_crosvm"
backup_file "$app_virgl" "$old_virgl_hash" "$backup_virgl"

echo "PID=$pid"
echo "CMDLINE=$cmdline"
stat -c 'ACTIVE=%n OWNER=%u:%g MODE=%a SIZE=%s' "$app_crosvm" "$app_virgl"
ls -Zd "$app_crosvm" "$app_virgl"
echo "ACTIVE_CROSVM_SHA256=$old_crosvm_hash"
echo "ACTIVE_VIRGL_SHA256=$old_virgl_hash"
echo "STAGED_CROSVM_SHA256=$new_crosvm_hash"
echo "STAGED_VIRGL_SHA256=$new_virgl_hash"
echo "BACKUP_CROSVM=$backup_crosvm"
echo "BACKUP_VIRGL=$backup_virgl"
echo "VALIDATION_ONLY=1"
