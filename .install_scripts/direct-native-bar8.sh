#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
crosvm=$app_root/usr/bin/crosvm
libpath=$app_root/usr/lib:$app_root/lib
image=/data/local/tmp/droidvm-sda17/win11-droidvm-final-comp.native-working-58052.qcow2
socket=$app_root/run/s-native-direct-bar8.sock
log=/data/local/tmp/crosvm-native-direct-bar8.log
tap=vmd1ea2c7c-0
bridge=brd75f2fb9

test -x "$crosvm"
test -f "$image"
test -z "$(pidof crosvm 2>/dev/null || true)"
rm -f "$log"
rm -f "$socket"

# The daemon normally creates and attaches this tap before launching crosvm. A root
# diagnostic launch has to reproduce that part or the guest boots without networking.
if ! ip link show "$tap" >/dev/null 2>&1; then
    ip tuntap add dev "$tap" mode tap vnet_hdr
fi
ip link set "$tap" master "$bridge"
ip link set "$tap" up

LD_LIBRARY_PATH="$libpath" "$crosvm" --extended-status run \
    --name s-native-direct-bar8 \
    --mem 2048 \
    --cpus 8 \
    --gpu-cgroup-path /dev/cpuset/gpuworker \
    --hypervisor gunyah \
    --pre-alloc drm-host-mb=8 \
    --protected-vm-pseudo-unprotected \
    --no-balloon \
    --disable-sandbox \
    --hugepages \
    --prepare-lend-mthp-mode chunked \
    --socket "$socket" \
    --smbios processor-version=SM8750P \
    --block "$image,lock=false" \
    --net tap-name=$tap,mac=02:be:ee:a7:46:dd \
    --gpu 'virglrenderer,displays=[[mode=windowed[1920,1080],refresh-rate=60,dpi=[160,160]]],context-types=drm,vulkan=false,egl=true,gles=true,udmabuf=true,fixed-blob-mapping=true,pci-bar-size=8388608' \
    --simplefb width=1920,height=1080 \
    --vnc-server host=127.0.0.1,port=5901,input=tablet \
    "$app_root/usr/share/droidvm/edk2-gunyah.fd" \
    >"$log" 2>&1 &

launcher=$!
attempt=0
while [ "$attempt" -lt 60 ]; do
    pid=$(pidof crosvm 2>/dev/null || true)
    if [ -n "$pid" ]; then
        test "$(echo "$pid" | wc -w)" -eq 1
        echo "crosvm_pid=$pid"
        tr '\000' ' ' <"/proc/$pid/cmdline"
        exit 0
    fi
    if ! kill -0 "$launcher" 2>/dev/null; then
        cat "$log" >&2 || true
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 1
done

cat "$log" >&2 || true
exit 1
