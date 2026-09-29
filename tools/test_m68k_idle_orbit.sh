#!/usr/bin/env bash
# Exact 68000 idle-orbit deferral vs chunked reference execution (real C68K,
# Cortex-A9 under QEMU).
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-m68k-orbit-XXXXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
core_dir="$repo_dir/src/core/c68k"
"${CC:-cc}" -O2 -DC68K_GEN -DHAVE_STDINT_H \
  "$core_dir/gen68k.c" "$core_dir/c68k.c" "$core_dir/c68kexec.c" -o "$test_dir/gen68k"
(cd "$test_dir" && ./gen68k > generator.log)
flags=(-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DHAVE_STDINT_H)
for source in c68k c68kexec; do
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" "${flags[@]}" -O2 -I"$test_dir" \
    -c "$core_dir/$source.c" -o "$test_dir/$source.o"
done
"${ARM_CC:-arm-linux-gnueabihf-gcc}" "${flags[@]}" -std=gnu11 -O2 -static -Wall -Wextra -Werror \
  -Wno-unused-function "$repo_dir/tests/test_m68k_idle_orbit.c" \
  "$test_dir/c68k.o" "$test_dir/c68kexec.o" -o "$test_dir/test"
qemu-arm -cpu cortex-a9 "$test_dir/test"
