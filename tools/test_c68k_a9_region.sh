#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-c68k-a9-XXXXXXXX)
core_dir="$repo_dir/src/core/c68k"
"${CC:-cc}" -O2 -DC68K_GEN -DHAVE_STDINT_H \
  "$core_dir/gen68k.c" "$core_dir/c68k.c" "$core_dir/c68kexec.c" -o "$test_dir/gen68k"
(cd "$test_dir" && ./gen68k > generator.log)
flags=(-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DHAVE_STDINT_H)
for source in c68k c68kexec; do
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" "${flags[@]}" -O0 -I"$test_dir" \
    -c "$core_dir/$source.c" -o "$test_dir/$source.o"
done
"${ARM_CXX:-arm-linux-gnueabihf-g++}" "${flags[@]}" -std=c++17 -O2 -static \
  -pthread -Wall -Wextra "$repo_dir/tests/test_c68k_a9_region.cpp" \
  "$test_dir/c68k.o" "$test_dir/c68kexec.o" -o "$test_dir/test"
qemu-arm -cpu cortex-a9 "$test_dir/test"
printf 'A32 region test artifacts: %s\n' "$test_dir"
