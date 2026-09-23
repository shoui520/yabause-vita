#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/scu-host" "$test_dir/scu-arm"; rmdir -- "$test_dir"' EXIT
common=(-std=c11 -O2 -DHAVE_STDINT_H -DHAVE_SYS_TIME_H
  -ffunction-sections -fdata-sections -Wl,--gc-sections
  "$repo_dir/tests/test_scu_dsp_operation.c" "$repo_dir/src/core/scu.c")
"${CC:-cc}" "${common[@]}" -O0 -fsanitize=undefined,address -fno-omit-frame-pointer -o "$test_dir/scu-host"
UBSAN_OPTIONS=halt_on_error=1 "$test_dir/scu-host"
"${ARM_CC:-arm-linux-gnueabihf-gcc}" "${common[@]}" -static -mcpu=cortex-a9 \
  -mfpu=neon -mfloat-abi=hard -o "$test_dir/scu-arm"
qemu-arm -cpu cortex-a9 "$test_dir/scu-arm"
