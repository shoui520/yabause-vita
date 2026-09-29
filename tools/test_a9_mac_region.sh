#!/usr/bin/env bash
# Region MAC.L/MAC.W versus the real MAC_L/MAC_W templates (VITA_SH2_MACL_WRAM/MACW_WRAM).
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/a9-mac"; rmdir -- "$test_dir"' EXIT
"${ARM_CXX:-arm-linux-gnueabihf-g++}" -std=c++17 -O2 -g -static -marm \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror -Wl,-z,noexecstack \
  -Wa,-I,"$repo_dir/src/core" -DVITA_SH2_MACL_WRAM -Wa,-defsym,VITA_SH2_MACL_WRAM=1 \
  -DVITA_SH2_MACW_WRAM -Wa,-defsym,VITA_SH2_MACW_WRAM=1 \
  "$repo_dir/tests/test_a9_mac_region.cpp" "$repo_dir/src/core/sh2_dynarec/dynalib_arm.s" \
  -o "$test_dir/a9-mac"
qemu-arm -cpu cortex-a9 "$test_dir/a9-mac"
