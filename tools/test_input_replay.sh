#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/input"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_input_replay.c" -o "$test_dir/input"
"$test_dir/input"
