#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/pattern"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -O2 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer --param asan-globals=0 \
  -I"$repo_dir/src/core" "$repo_dir/tests/test_rotation_pattern_cache.c" \
  -Wl,--gc-sections -lm -o "$test_dir/pattern"
"$test_dir/pattern"
