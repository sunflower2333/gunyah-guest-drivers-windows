#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workspace=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/nctx-wire.XXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT

source_file="$script_dir/abi_manifest.cpp"
expected="$script_dir/expected-v2.txt"
common_flags=(-std=gnu++17 -Wall -Wextra -Werror -I"$script_dir")

run_manifest()
{
    local name=$1
    shift
    local binary="$tmp_dir/$name"
    local output="$tmp_dir/$name.txt"

    "$@" "$source_file" -o "$binary"
    "$binary" >"$output"
    diff -u "$expected" "$output"
    echo "PASS $name"
}

host_includes=(
    -I"$workspace/virglrenderer/src"
    -I"$workspace/virglrenderer/src/drm"
    -I"$workspace/virglrenderer/src/drm/msm"
    -I"$workspace/virglrenderer/src/drm/drm-uapi"
)
guest_includes=(
    -I"$workspace/mesa/src/virtio/virtio-gpu"
    -I"$workspace/mesa/src/freedreno/common"
    -I"$workspace/mesa/include/drm-uapi"
)
windows_includes=(-I"$workspace/gunyah-guest-drivers-windows/viogpu/common")

run_manifest host-gcc g++ "${common_flags[@]}" "${host_includes[@]}"
run_manifest host-clang clang++ "${common_flags[@]}" "${host_includes[@]}"
run_manifest guest-gcc g++ "${common_flags[@]}" "${guest_includes[@]}"
run_manifest guest-clang clang++ "${common_flags[@]}" "${guest_includes[@]}"
run_manifest windows-clang clang++ -DABI_ENDPOINT_WINDOWS \
    "${common_flags[@]}" "${windows_includes[@]}"

aarch64-linux-gnu-gcc -x c -std=gnu11 -Wall -Wextra -Werror -I"$script_dir" \
    "${guest_includes[@]}" \
    "$source_file" -o "$tmp_dir/guest-aarch64"
echo "PASS guest-aarch64 compile"
