#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-runtime-XXXXXXXX)
core_dir="$repo_dir/src/core/c68k"
"${CC:-cc}" -O2 -DC68K_GEN -DHAVE_STDINT_H \
  "$core_dir/gen68k.c" "$core_dir/c68k.c" "$core_dir/c68kexec.c" -o "$test_dir/gen68k"
(cd "$test_dir" && ./gen68k > generator.log)
flags=(-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DHAVE_STDINT_H -pthread
  -DVITA_M68K_NATIVE_GUARDS -DVITA_M68K_NATIVE_EXECUTION -DC68K_NATIVE_REGIONS
  -DVITA_M68K_CODE_RESERVE_KIB=1024)
for source in c68k c68kexec; do
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" "${flags[@]}" -O0 -I"$test_dir" \
    -c "$core_dir/$source.c" -o "$test_dir/$source.o"
done
"${ARM_CXX:-arm-linux-gnueabihf-g++}" "${flags[@]}" -O2 -std=c++17 -static \
  -Wall -Wextra -I"$repo_dir/tests/c68k_runtime_stubs" \
  "$repo_dir/tests/test_c68k_runtime.cpp" "$repo_dir/src/vita/c68k_runtime.cpp" \
  "$repo_dir/src/vita/c68k_native_guard.cpp" "$test_dir/c68k.o" "$test_dir/c68kexec.o" \
  -o "$test_dir/test"
qemu-arm -cpu cortex-a9 "$test_dir/test"
printf 'Runtime test artifacts: %s\n' "$test_dir"
