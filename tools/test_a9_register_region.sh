#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/a9-region" "$test_dir/oracle.o"; rmdir -- "$test_dir"' EXIT
"${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -O2 -g \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections \
  -I"$repo_dir/src/core" -c "$repo_dir/tests/sh2_counted_loop_oracle.c" \
  -o "$test_dir/oracle.o"
"${ARM_CXX:-arm-linux-gnueabihf-g++}" -std=c++17 -O2 -g -static \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  "$repo_dir/tests/test_a9_register_region.cpp" "$test_dir/oracle.o" \
  -Wl,--gc-sections -o "$test_dir/a9-region"
qemu-arm -cpu cortex-a9 "$test_dir/a9-region"
"${ARM_CXX:-arm-linux-gnueabihf-g++}" -std=c++17 -O2 -g -static -DVITA_SH2_DIV_CHAIN \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  "$repo_dir/tests/test_a9_register_region.cpp" "$test_dir/oracle.o" \
  -Wl,--gc-sections -o "$test_dir/a9-region"
qemu-arm -cpu cortex-a9 "$test_dir/a9-region"
