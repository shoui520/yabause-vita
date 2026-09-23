#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/generation" "$test_dir/writes"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -O2 -g -fsanitize=address,undefined \
  "$repo_dir/tests/test_vdp2_generation.c" -o "$test_dir/generation"
"$test_dir/generation"
"${CXX:-c++}" -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined --param asan-globals=0 \
  -DHAVE_STDINT_H -DVITA_ROTATION_PATTERN_CACHE -DVITA_ROTATION_OUTPUT_GENERATION \
  -I"$repo_dir/src/core" -I"$repo_dir/src/video/opengl" \
  "$repo_dir/tests/test_vdp2_generation_writes.cpp" "$repo_dir/src/core/vdp2.cpp" \
  -Wl,--gc-sections -o "$test_dir/writes"
"$test_dir/writes"
