#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d -t yabause-arith-tests.XXXXXXXX)"
# The temporary executable is retained so a failed test can be inspected.
"${ARM_LINUX_CC:-arm-linux-gnueabihf-gcc}" -static -O2 -mthumb \
  -mcpu=cortex-a9 -mfpu=neon -Wl,-z,noexecstack -Wa,-I,"$repo_root/src/core" \
  "$repo_root/tests/test_arm_arith_templates.c" \
  "$repo_root/src/core/sh2_dynarec/dynalib_arm.s" \
  -o "$test_dir/test-arm-arith-templates"
"${QEMU_ARM:-qemu-arm}" -cpu cortex-a9 "$test_dir/test-arm-arith-templates"
printf 'Test executable: %s\n' "$test_dir/test-arm-arith-templates"
