#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
common=(-std=gnu11 -O2 -DHAVE_STDINT_H -DHAVE_SYS_TIME_H -DVITA_SCU_DSP_FAST
  -ffunction-sections -fdata-sections -Wl,--gc-sections
  "$repo_dir/tests/test_scu_dsp_fast.c" "$repo_dir/src/core/scu.c")
"${CC:-cc}" "${common[@]}" -fsanitize=undefined -fno-sanitize=bounds,signed-integer-overflow,shift -fno-omit-frame-pointer -o "$test_dir/dsp-host"
UBSAN_OPTIONS=halt_on_error=1 "$test_dir/dsp-host"
if command -v "${ARM_CC:-arm-linux-gnueabihf-gcc}" >/dev/null && command -v qemu-arm >/dev/null; then
  "${ARM_CC:-arm-linux-gnueabihf-gcc}" "${common[@]}" -static -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -o "$test_dir/dsp-arm"
  qemu-arm -cpu cortex-a9 "$test_dir/dsp-arm"
fi
