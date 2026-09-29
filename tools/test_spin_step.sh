#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/spin-step"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror "$repo_dir/tests/test_spin_step.c" -o "$test_dir/spin-step"
"$test_dir/spin-step"
