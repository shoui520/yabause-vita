#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/framebuffer"; rmdir -- "$test_dir"' EXIT
for retain in 0 1; do
defines=()
if [[ "$retain" == 1 ]]; then defines+=(-DVITA_VDP1_RETAIN_ATTACHMENTS -DVITA_VDP1_RETAIN_COLOR); fi
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror "${defines[@]}" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vitagl_framebuffer.c" -lm -o "$test_dir/framebuffer"
"$test_dir/framebuffer"
done
