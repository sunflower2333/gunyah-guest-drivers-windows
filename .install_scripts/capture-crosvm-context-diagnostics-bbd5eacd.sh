#!/system/bin/sh
set -eu

mode=${1:-}
start_line=${2:-}
expected_inode=${3:-}
case "$mode" in
    --baseline)
        test "$#" -eq 1 || { echo "usage: $0 --baseline|--capture START_LINE INODE" >&2; exit 2; }
        ;;
    --capture)
        test "$#" -eq 3 || { echo "usage: $0 --baseline|--capture START_LINE INODE" >&2; exit 2; }
        case "$start_line" in
            ''|*[!0-9]*) echo "START_LINE must be a decimal integer" >&2; exit 2 ;;
        esac
        case "$expected_inode" in
            ''|*[!0-9]*) echo "INODE must be a decimal integer" >&2; exit 2 ;;
        esac
        ;;
    *) echo "usage: $0 --baseline|--capture START_LINE INODE" >&2; exit 2 ;;
esac

if [ "$(id -u)" != "0" ]; then
    if [ "$mode" = "--baseline" ]; then
        exec su -c "/system/bin/sh $0 --baseline"
    fi
    exec su -c "/system/bin/sh $0 --capture $start_line $expected_inode"
fi

app_root=/data/data/cn.classfun.droidvm
app_crosvm=$app_root/usr/bin/crosvm
app_virgl=$app_root/usr/lib/libvirglrenderer.so
vm_id=a5b6863f-6bbf-4af4-97f3-2a4653f6edb7
stderr_log=$app_root/cache/console_${vm_id}_stderr.log
expected_crosvm_hash=bbd5eacd4d430e68c2fe497be681d1367f590a07d86eb7b60179d8a002ef53dc
expected_virgl_hash=77a99067ec8fde12a8261865821f9f6da0f024c5f176663c83dd2214fdc145af

set -- $(pidof crosvm 2>/dev/null || true)
if [ "$#" -ne 1 ]; then
    echo "expected exactly one crosvm process, found $#" >&2
    exit 2
fi

pid=$1
cmdline=$(xargs -0 < "/proc/$pid/cmdline")
for fragment in \
    "--name s" \
    "--mem 2048" \
    "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2" \
    "context-types=drm" \
    "udmabuf=true" \
    "fixed-blob-mapping=true"; do
    case "$cmdline" in
        *"$fragment"*) ;;
        *) echo "missing required crosvm argument: $fragment" >&2; exit 2 ;;
    esac
done

crosvm_hash=$(sha256sum "$app_crosvm" | awk '{print $1}')
virgl_hash=$(sha256sum "$app_virgl" | awk '{print $1}')
if [ "$crosvm_hash" != "$expected_crosvm_hash" ] || \
   [ "$virgl_hash" != "$expected_virgl_hash" ]; then
    echo "diagnostic Host hashes are not active" >&2
    echo "CROSVM_SHA256=$crosvm_hash" >&2
    echo "VIRGL_SHA256=$virgl_hash" >&2
    exit 3
fi

test -f "$stderr_log"
inode=$(stat -c '%i' "$stderr_log")
line_count=$(wc -l < "$stderr_log")

if [ "$mode" = "--baseline" ]; then
    echo "PID=$pid"
    echo "CROSVM_SHA256=$crosvm_hash"
    echo "VIRGL_SHA256=$virgl_hash"
    echo "STDERR_LOG=$stderr_log"
    stat -c 'STDERR_SIZE=%s STDERR_MTIME=%y' "$stderr_log"
    echo "BASELINE_LINE=$line_count"
    echo "BASELINE_INODE=$inode"
    echo 'BASELINE_COMPLETE=1'
    exit 0
fi

if [ "$inode" != "$expected_inode" ]; then
    echo "UUID stderr log inode changed: expected $expected_inode, got $inode" >&2
    exit 4
fi
if [ "$line_count" -lt "$start_line" ]; then
    echo "UUID stderr log was truncated: baseline $start_line, current $line_count" >&2
    exit 4
fi
first_line=$((start_line + 1))

count_marker()
{
    label=$1
    pattern=$2
    count=$(tail -n +"$first_line" "$stderr_log" | grep -c -E "$pattern" || true)
    echo "$label=$count"
}

echo "PID=$pid"
echo "CROSVM_SHA256=$crosvm_hash"
echo "VIRGL_SHA256=$virgl_hash"
echo "STDERR_LOG=$stderr_log"
stat -c 'STDERR_SIZE=%s STDERR_MTIME=%y' "$stderr_log"
echo "BASELINE_LINE=$start_line"
echo "BASELINE_INODE=$expected_inode"
echo "CAPTURE_LINE=$line_count"
echo "CAPTURE_INODE=$inode"

count_marker VA_SLICE_CLAIM 'VA slice [0-9]+: \['
count_marker CONTEXT_TABLE_ADD 'virgl context table add:.*result=ok'
count_marker RUTABAGA_DESTROY_BEGIN 'rutabaga context destroy begin:'
count_marker RUST_DROP_BEGIN 'virglrenderer context drop begin:'
count_marker VIRGL_TABLE_REMOVE_BEGIN 'virgl context table remove begin:'
count_marker VIRGL_TABLE_FOUND 'virgl context table remove begin:.*found=1'
count_marker VIRGL_BACKEND_DESTROY_BEGIN 'virgl context backend destroy begin:'
count_marker KGSL_BACKEND_DESTROY_BEGIN 'ctx_id=.*backend destroy begin VA slice='
count_marker VA_SLICE_RELEASE 'ctx_id=.*released VA slice .*used=0x'
count_marker KGSL_BACKEND_DESTROY_COMPLETE 'ctx_id=.*backend destroy complete'
count_marker VIRGL_BACKEND_DESTROY_COMPLETE 'virgl context backend destroy complete:'
count_marker VIRGL_TABLE_REMOVE_COMPLETE 'virgl context table remove complete:.*found=1'
count_marker RUST_DROP_COMPLETE 'virglrenderer context drop complete:'
count_marker RUTABAGA_DESTROY_COMPLETE 'rutabaga context destroy complete:'

echo '===== FILTERED_CONTEXT_TEARDOWN ====='
tail -n +"$first_line" "$stderr_log" | grep -n -E \
    'VA slice|rutabaga context destroy|virglrenderer context drop|virgl context table add|virgl context table remove|virgl context backend destroy|ctx_id=.*backend destroy|ctx_id=.*released VA slice|drm2kgsl_renderer_create' || true
echo 'CAPTURE_COMPLETE=1'
