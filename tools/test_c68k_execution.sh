#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-c68k-exec-XXXXXXXX)
# Keep generated sources and executables for diagnosis and disassembly.
core_dir="$repo_dir/src/core/c68k"
"${CC:-cc}" -O2 -DC68K_GEN -DHAVE_STDINT_H \
  "$core_dir/gen68k.c" "$core_dir/c68k.c" "$core_dir/c68kexec.c" \
  -o "$test_dir/gen68k"
(cd "$test_dir" && ./gen68k > generator.log)
for target in host arm; do
  compiler=("${CC:-cc}")
  # O0 gives a quick contract check; use C68K_TEST_OPT=-O2 for the product
  # optimization level (this large generated executor takes longer to build).
  flags=("${C68K_TEST_OPT:--O0}" -g -fno-gcse -fno-crossjumping -DHAVE_STDINT_H)
  if [[ $target == arm ]]; then
    compiler=("${ARM_CC:-arm-linux-gnueabihf-gcc}")
    flags+=(-static -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  fi
  "${compiler[@]}" "${flags[@]}" -I"$test_dir" \
    "$repo_dir/tests/test_c68k_execution.c" "$core_dir/c68k.c" \
    "$core_dir/c68kexec.c" -o "$test_dir/$target"
  if [[ $target == arm ]]; then
    qemu-arm -cpu cortex-a9 "$test_dir/$target"
  else
    "$test_dir/$target"
  fi
done
printf 'Executor test artifacts: %s\n' "$test_dir"
