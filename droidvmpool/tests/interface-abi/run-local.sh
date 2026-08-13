#!/usr/bin/env bash
set -euo pipefail

test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

compiler_count=0
for compiler in g++ clang++; do
  if ! command -v "${compiler}" >/dev/null 2>&1; then
    continue
  fi
  compiler_count=$((compiler_count + 1))
  "${compiler}" -std=c++14 -Wall -Wextra -Werror "${test_dir}/abi_manifest.cpp" -o "${temp_dir}/abi-${compiler}"
  "${temp_dir}/abi-${compiler}" >"${temp_dir}/actual-${compiler}.txt"
  diff -u "${test_dir}/expected-v1.txt" "${temp_dir}/actual-${compiler}.txt"
  echo "PASS ${compiler}"
done

if [[ "${compiler_count}" -eq 0 ]]; then
  echo "no supported C++ compiler found" >&2
  exit 1
fi
