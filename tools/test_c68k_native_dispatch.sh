#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-c68k-dispatch-XXXXXXXX)
core_dir="$repo_dir/src/core/c68k"
table_flags=()
case "${C68K_TEST_TABLE:-dynamic}" in
  dynamic) ;;
  constant) table_flags=(-DC68K_CONST_JUMP_TABLE) ;;
  switch) table_flags=(-DC68K_NO_JUMP_TABLE) ;;
  *) printf 'Invalid C68K_TEST_TABLE\n' >&2; exit 2 ;;
esac
"${CC:-cc}" "${table_flags[@]}" -O2 -DC68K_GEN -DHAVE_STDINT_H \
  "$core_dir/gen68k.c" "$core_dir/c68k.c" "$core_dir/c68kexec.c" -o "$test_dir/gen68k"
(cd "$test_dir" && ./gen68k > generator.log)
flags=(-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DHAVE_STDINT_H -DC68K_NATIVE_REGIONS -DVITA_M68K_NATIVE_GUARDS -pthread)
flags+=("${table_flags[@]}")
for source in c68k c68kexec; do
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" "${flags[@]}" -O0 -I"$test_dir" \
    -c "$core_dir/$source.c" -o "$test_dir/$source.o"
done
"${ARM_CC:-arm-linux-gnueabihf-gcc}" "${flags[@]}" -O2 \
  -c "$repo_dir/tests/test_c68k_execution.c" -o "$test_dir/contracts.o"
"${ARM_CXX:-arm-linux-gnueabihf-g++}" "${flags[@]}" -std=c++17 -O2 -static \
  -Wall -Wextra "$repo_dir/tests/c68k_native_test_backend.cpp" \
  "$repo_dir/src/vita/c68k_native_guard.cpp" \
  "$test_dir/c68k.o" "$test_dir/c68kexec.o" "$test_dir/contracts.o" -o "$test_dir/test"
qemu-arm -cpu cortex-a9 "$test_dir/test"
printf 'Native dispatch test artifacts: %s\n' "$test_dir"
