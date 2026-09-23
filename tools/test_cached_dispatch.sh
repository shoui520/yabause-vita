#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/dispatch"; rmdir -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_cached_dispatch.cpp" -o "$test_dir/dispatch"
"$test_dir/dispatch"
