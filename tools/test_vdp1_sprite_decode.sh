#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/sprite" "$test_dir/sprite-arm"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_vdp1_sprite_decode.c" -o "$test_dir/sprite"
"$test_dir/sprite"
if command -v "${ARM_CC:-arm-linux-gnueabihf-gcc}" >/dev/null && command -v qemu-arm >/dev/null; then
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -O2 -static -mcpu=cortex-a9 -mfpu=neon \
    -Wall -Wextra -Werror "$repo_dir/tests/test_vdp1_sprite_decode.c" -o "$test_dir/sprite-arm"
  qemu-arm -cpu cortex-a9 "$test_dir/sprite-arm"
fi
