#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/chain"; rmdir -- "$test_dir"' EXIT
"${ARM_CXX:-arm-linux-gnueabihf-g++}" -std=c++17 -O2 -static -mcpu=cortex-a9 \
  -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  -Wa,-defsym,VITA_SH2_CHAIN_ABI=1 -Wa,-I,"$repo_dir/src/core" -Wl,-z,noexecstack \
  "$repo_dir/tests/test_chain_abi.cpp" "$repo_dir/tests/test_chain_entry.s" \
  "$repo_dir/src/core/sh2_dynarec/dynalib_arm.s" -o "$test_dir/chain"
qemu-arm -cpu cortex-a9 "$test_dir/chain"
