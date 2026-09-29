#!/usr/bin/env bash
# SH2Div64 (SH-2 64/32 DIVU) against C's / and %: must be bit-identical.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d -t yabause-div64-tests.XXXXXXXX)"
"${ARM_LINUX_CC:-arm-linux-gnueabihf-gcc}" -std=gnu11 -static -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -DHAVE_STDINT_H -I "$repo_root/src/core" "$repo_root/tests/test_sh2_div64.c" -o "$test_dir/test-div64"
"${QEMU_ARM:-qemu-arm}" -cpu cortex-a9 "$test_dir/test-div64" "${1:-2000000}"
printf 'Test executable: %s\n' "$test_dir"
