#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-c68k-cache-XXXXXXXX)
source="$repo_dir/tests/test_c68k_region_cache.cpp"
"${CXX:-c++}" -std=c++17 -O2 -pthread -Wall -Wextra \
  -fsanitize=address,undefined "$source" -o "$test_dir/host"
"$test_dir/host"
"${ARM_CXX:-arm-linux-gnueabihf-g++}" -std=c++17 -O2 -pthread -static \
  -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wextra \
  "$source" -o "$test_dir/arm"
qemu-arm -cpu cortex-a9 "$test_dir/arm"
printf 'Region cache test artifacts: %s\n' "$test_dir"
