#!/usr/bin/env bash
# MAC.L template with and without VITA_SH2_MACL_WRAM: identical digests required.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d -t yabause-macl-tests.XXXXXXXX)"
for variant in callback inline; do
  flags=()
  [ "$variant" = inline ] && flags=(-DVITA_SH2_MACL_WRAM -Wa,-defsym,VITA_SH2_MACL_WRAM=1)
  "${ARM_LINUX_CC:-arm-linux-gnueabihf-gcc}" -static -O2 -mthumb \
    -mcpu=cortex-a9 -mfpu=neon -Wl,-z,noexecstack -Wa,-I,"$repo_root/src/core" "${flags[@]}" \
    "$repo_root/tests/test_macl_template.c" "$repo_root/src/core/sh2_dynarec/dynalib_arm.s" \
    -o "$test_dir/test-macl-$variant"
  "${QEMU_ARM:-qemu-arm}" -cpu cortex-a9 "$test_dir/test-macl-$variant" | tee "$test_dir/$variant.txt"
done
a=$(sed 's/ (inline WRAM)//' "$test_dir/callback.txt"); b=$(sed 's/ (inline WRAM)//' "$test_dir/inline.txt")
[ "$a" = "$b" ] && echo "MAC.L templates identical" || { echo "MAC.L templates DIFFER"; exit 1; }
printf 'Test executables: %s\n' "$test_dir"
