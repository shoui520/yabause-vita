#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vita_config.c" "$repo_dir/src/vita/config.c" \
  "$repo_dir/src/vita/game_list.c" -o "$test_dir/config"
mkdir "$test_dir/data"
"$test_dir/config" "$test_dir/data"
