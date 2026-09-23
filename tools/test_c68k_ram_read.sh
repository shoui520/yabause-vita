#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
test_binary="$test_dir/host"
trap 'rm -f -- "$test_dir/host" "$test_dir/arm"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=gnu11 -O2 -DHAVE_STDINT_H -DVITA_C68K_RAM_READS \
  -Wall -Wextra -fsanitize=address,undefined \
  "$repo_dir/tests/test_c68k_ram_read.c" "$repo_dir/src/core/c68k/c68k.c" -o "$test_binary"
"$test_binary"
"${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -O2 -static \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DHAVE_STDINT_H -DVITA_C68K_RAM_READS \
  -Wall -Wextra "$repo_dir/tests/test_c68k_ram_read.c" \
  "$repo_dir/src/core/c68k/c68k.c" -o "$test_dir/arm"
qemu-arm -cpu cortex-a9 "$test_dir/arm"
