#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/scu-host" "$test_dir/scu-arm"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  "$repo_dir/tests/test_scu_dsp_arithmetic.c" -o "$test_dir/scu-host"
"$test_dir/scu-host"
"${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=c11 -O3 -static -mcpu=cortex-a9 \
  -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  "$repo_dir/tests/test_scu_dsp_arithmetic.c" -o "$test_dir/scu-arm"
qemu-arm -cpu cortex-a9 "$test_dir/scu-arm"
