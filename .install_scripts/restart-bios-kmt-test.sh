#!/system/bin/sh
set -eu

APP=/data/data/cn.classfun.droidvm
CROSVM=$APP/usr/bin/crosvm
SOCKET=/data/local/tmp/s-bios-kmt.sock
LOG=/data/local/tmp/s-bios-kmt.log
IMAGE=/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2
TAP=vma5b6863f0-0

pid=$(pidof crosvm 2>/dev/null || true)
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1

if [ -S /data/data/cn.classfun.droidvm/run/s.sock ]; then
    LD_LIBRARY_PATH=$APP/usr/lib "$CROSVM" stop /data/data/cn.classfun.droidvm/run/s.sock
else
    kill "$pid"
fi
for _ in $(seq 1 60); do
    pid=$(pidof crosvm 2>/dev/null || true)
    [ -z "$pid" ] && break
    sleep 1
done
test -z "$(pidof crosvm 2>/dev/null || true)"
rm -f "$SOCKET" "$LOG"

export LD_LIBRARY_PATH=$APP/usr/lib
export RUST_LOG=warn
export CROSVM_DRM2KGSL_DIAG=1
nohup "$CROSVM" --extended-status run \
  --name s-bios-kmt --mem 2048 --cpus 8 \
  --gpu-cgroup-path /dev/cpuset/gpuworker --hypervisor gunyah \
  --pre-alloc drm-host-mb=8 --protected-vm-pseudo-unprotected --no-balloon \
  --disable-sandbox --hugepages --prepare-lend-mthp-mode chunked \
  --socket "$SOCKET" --smbios processor-version=SM8750P \
  --block "$IMAGE,lock=false" \
  --net "tap-name=$TAP,mac=02:be:ee:a7:46:dd" \
  --gpu 'virglrenderer,displays=[[mode=windowed[1920,1080],refresh-rate=60,dpi=[160,160]]],context-types=drm,vulkan=false,egl=true,gles=true,udmabuf=true,fixed-blob-mapping=true,pci-bar-size=8388608' \
  --vnc-server host=127.0.0.1,port=5900,input=tablet \
  --serial type=file,hardware=serial,num=1,earlycon,console,path=/data/local/tmp/s-bios-kmt.serial.log \
  --bios "$APP/usr/share/droidvm/edk2-gunyah.fd" >"$LOG" 2>&1 </dev/null &

pid=
for _ in $(seq 1 90); do
    pid=$(pidof crosvm 2>/dev/null || true)
    [ -n "$pid" ] && break
    sleep 1
done
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
echo "crosvm_pid=$pid"
echo "log=$LOG"
