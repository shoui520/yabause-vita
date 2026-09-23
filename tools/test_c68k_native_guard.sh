#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-c68k-guard-XXXXXXXX)
for target in host arm; do
  cc=("${CC:-cc}"); cxx=("${CXX:-c++}")
  flags=(-O2 -g -pthread -DVITA_M68K_NATIVE_GUARDS -Wall -Wextra)
  if [[ $target == arm ]]; then
    cc=("${ARM_CC:-arm-linux-gnueabihf-gcc}"); cxx=("${ARM_CXX:-arm-linux-gnueabihf-g++}")
    flags+=(-static -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  else flags+=(-fsanitize=address,undefined)
  fi
  "${cc[@]}" "${flags[@]}" -std=c11 -c "$repo_dir/tests/c68k_guard_c_fixture.c" -o "$test_dir/$target.o"
  "${cxx[@]}" "${flags[@]}" -std=c++17 "$repo_dir/tests/test_c68k_native_guard.cpp" \
    "$repo_dir/src/vita/c68k_native_guard.cpp" "$test_dir/$target.o" -o "$test_dir/$target"
  if [[ $target == arm ]]; then qemu-arm -cpu cortex-a9 "$test_dir/$target"; else "$test_dir/$target"; fi
done
printf 'Guard test artifacts: %s\n' "$test_dir"
