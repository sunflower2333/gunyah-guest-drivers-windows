#!/system/bin/sh
set -eu
root=/data/data/cn.classfun.droidvm/cache
grep -R -n -E 'GPU-POOL-EARLY|GPU-CREATE-BLOB|GPU-MAPBLOB|drm2kgsl:|fixed blob|prebacked|BAR' "$root" 2>/dev/null | tail -n 300 || true
