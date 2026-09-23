#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/a9-ram"; rmdir -- "$test_dir"' EXIT
"${ARM_CXX:-arm-linux-gnueabihf-g++}" -std=c++17 -O2 -g -static \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  "$repo_dir/tests/test_a9_ram_load.cpp" -o "$test_dir/a9-ram"
qemu-arm -cpu cortex-a9 "$test_dir/a9-ram"
