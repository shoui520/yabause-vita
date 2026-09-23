#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-code-layout-XXXXXXXX)
for reserve in 0 4 1024 4096 8192 15360; do
  "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
    -DVITA_M68K_CODE_RESERVE_KIB="$reserve" "$repo_dir/tests/test_code_arena_layout.cpp" \
    -o "$test_dir/test-$reserve"
  "$test_dir/test-$reserve"
done
for reserve in 0 1024; do
  "${ARM_CXX:-arm-linux-gnueabihf-g++}" -std=c++17 -O2 -static -marm \
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wextra \
    -DVITA_M68K_CODE_RESERVE_KIB="$reserve" "$repo_dir/tests/test_code_arena_layout.cpp" \
    -o "$test_dir/arm-$reserve"
  qemu-arm -cpu cortex-a9 "$test_dir/arm-$reserve"
done
for reserve in -4 1 16384; do
  if "${CXX:-c++}" -std=c++17 -DVITA_M68K_CODE_RESERVE_KIB="$reserve" \
    -c "$repo_dir/tests/test_code_arena_layout.cpp" -o "$test_dir/invalid.o" \
    > "$test_dir/invalid-$reserve.log" 2>&1; then
    printf 'Invalid reserve accepted: %s\n' "$reserve" >&2
    exit 1
  fi
done
printf 'Code-layout test artifacts: %s\n' "$test_dir"
