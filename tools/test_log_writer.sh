#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/asan" "$test_dir/arm"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  "$repo_dir/tests/test_log_writer.c" -o "$test_dir/asan"
"$test_dir/asan"
if command -v "${ARM_CC:-arm-linux-gnueabihf-gcc}" >/dev/null && command -v qemu-arm >/dev/null; then
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -O2 -static -pthread -mcpu=cortex-a9 \
    -Wall -Wextra -Werror "$repo_dir/tests/test_log_writer.c" -o "$test_dir/arm"
  qemu-arm -cpu cortex-a9 "$test_dir/arm"
fi
