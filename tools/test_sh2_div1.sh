#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/div1"; rmdir -- "$test_dir"' EXIT
"${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -O2 -g -static \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections \
  -I"$repo_dir/src/core" "$repo_dir/tests/test_sh2_div1.c" \
  "$repo_dir/tests/sh2_counted_loop_oracle.c" "$repo_dir/tests/sh2_div1_a9.S" \
  -Wl,--gc-sections -o "$test_dir/div1"
qemu-arm -cpu cortex-a9 "$test_dir/div1"
