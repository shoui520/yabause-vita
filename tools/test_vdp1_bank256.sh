#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
# Vdp1MaskSpritePixel, verbatim from vidogl.c (static INLINE ... through its closing brace).
awk '/^static INLINE void Vdp1MaskSpritePixel\(/{p=1} p{print} p&&/^}/{exit}' \
  "$repo_dir/src/video/opengl/vidogl.c" > "$test_dir/vdp1_mask_extract.inc"
grep -q "case 0xF:" "$test_dir/vdp1_mask_extract.inc"
# The fast path itself, verbatim.
awk '/BANK256_BEGIN/{p=1} p{print} /BANK256_END/{exit}' \
  "$repo_dir/src/video/opengl/vidogl.c" > "$test_dir/bank256_extract.inc"
grep -q "BANK256_END" "$test_dir/bank256_extract.inc"
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -fno-strict-aliasing -fsanitize=address,undefined \
  -I"$test_dir" "$repo_dir/tests/test_vdp1_bank256.c" -o "$test_dir/bank256"
"$test_dir/bank256"
if command -v "${ARM_CC:-arm-linux-gnueabihf-gcc}" >/dev/null && command -v qemu-arm >/dev/null; then
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -O2 -static -mcpu=cortex-a9 -mfpu=neon -fno-strict-aliasing \
    -Wall -Wextra -Werror -I"$test_dir" "$repo_dir/tests/test_vdp1_bank256.c" -o "$test_dir/bank256-arm"
  qemu-arm -cpu cortex-a9 "$test_dir/bank256-arm"
fi
