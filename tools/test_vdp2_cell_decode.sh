#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/cell" "$test_dir/cell-arm"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_vdp2_cell_decode.c" -o "$test_dir/cell"
"$test_dir/cell"
if command -v "${ARM_CC:-arm-linux-gnueabihf-gcc}" >/dev/null && command -v qemu-arm >/dev/null; then
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -O2 -static -mcpu=cortex-a9 -mfpu=neon \
    -Wall -Wextra -Werror "$repo_dir/tests/test_vdp2_cell_decode.c" -o "$test_dir/cell-arm"
  qemu-arm -cpu cortex-a9 "$test_dir/cell-arm"
fi
