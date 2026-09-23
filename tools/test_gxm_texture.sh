#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/texture"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_gxm_texture.c" "$repo_dir/src/vita/gxm/texture.c" \
  -o "$test_dir/texture"
"$test_dir/texture"
