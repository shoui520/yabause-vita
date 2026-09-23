#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -t yabause-affinity-XXXXXXXX)
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -fsanitize=undefined,address \
  "$repo_dir/tests/test_cpu_affinity.c" -o "$test_dir/test"
"$test_dir/test"
printf 'Affinity test artifacts: %s\n' "$test_dir"
