#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_binary=$(mktemp)
trap 'rm -f -- "$test_binary"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_native_hot_samples.cpp" -o "$test_binary"
"$test_binary"
