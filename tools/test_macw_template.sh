#!/usr/bin/env bash
# MAC.W template vs its callback semantics, with and without VITA_SH2_MACW_WRAM.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d -t yabause-macw-tests.XXXXXXXX)"
for variant in callback inline; do
  flags=()
  [ "$variant" = inline ] && flags=(-DVITA_SH2_MACW_WRAM -Wa,-defsym,VITA_SH2_MACW_WRAM=1)
  "${ARM_LINUX_CC:-arm-linux-gnueabihf-gcc}" -static -O2 -mthumb \
    -mcpu=cortex-a9 -mfpu=neon -Wl,-z,noexecstack -Wa,-I,"$repo_root/src/core" "${flags[@]}" \
    "$repo_root/tests/test_macw_template.c" "$repo_root/src/core/sh2_dynarec/dynalib_arm.s" \
    -o "$test_dir/test-macw-$variant"
  "${QEMU_ARM:-qemu-arm}" -cpu cortex-a9 "$test_dir/test-macw-$variant"
done
printf 'Test executables: %s\n' "$test_dir"
