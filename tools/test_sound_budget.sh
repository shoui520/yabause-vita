#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/sound-budget"; rmdir -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -g -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined "$repo_dir/tests/test_sound_budget.cpp" \
  -o "$test_dir/sound-budget"
timeout 20 "$test_dir/sound-budget"
