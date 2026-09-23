#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/byte"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -O2 -g -fsanitize=address,undefined \
  "$repo_dir/tests/test_rotation_byte_vram.c" -o "$test_dir/byte"
"$test_dir/byte"
