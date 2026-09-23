#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/palette"; rmdir -- "$test_dir"' EXIT
# Global ASan registration would retain the unused frontend's function table.
# Test VRAM/CRAM use guarded heap allocations; the palette uses the guarded stack.
"${CC:-cc}" -O2 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  --param asan-globals=0 \
  -I"$repo_dir/src/core" "$repo_dir/tests/test_vdp2_palette.c" \
  -Wl,--gc-sections -lm -o "$test_dir/palette"
"$test_dir/palette"
