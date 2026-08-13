#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workspace=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/wddm-private-abi.XXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT

source_file="$script_dir/abi_manifest.cpp"
expected="$script_dir/expected-pre-v1.txt"
common_flags=(-std=c++17 -Wall -Wextra -Werror -I"$script_dir" -I"$workspace/viogpu/shared")

run_manifest()
{
    local endpoint=$1
    local compiler=$2
    local define=$3
    local binary="$tmp_dir/$endpoint-$compiler"
    local output="$tmp_dir/$endpoint-$compiler.txt"

    "$compiler" "${common_flags[@]}" "$define" "$source_file" -o "$binary"
    "$binary" >"$output"
    diff -u "$expected" "$output"
    echo "PASS $endpoint-$compiler"
}

run_manifest kmd g++ -DABI_ENDPOINT_KMD
run_manifest kmd clang++ -DABI_ENDPOINT_KMD
run_manifest umd g++ -DABI_ENDPOINT_UMD
run_manifest umd clang++ -DABI_ENDPOINT_UMD
