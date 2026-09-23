#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/scsp-notify" "$test_dir/scsp-notify-arm"; rmdir -- "$test_dir"' EXIT
cc -std=c11 -O0 -g -DHAVE_STDINT_H -DHAVE_SYS_TIME_H \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_scsp_dsp_write_notify.c" \
  "$repo_dir/src/core/scspdsp.c" -o "$test_dir/scsp-notify"
"$test_dir/scsp-notify"
"${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=c11 -O2 -static \
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -DHAVE_STDINT_H -DHAVE_SYS_TIME_H \
  "$repo_dir/tests/test_scsp_dsp_write_notify.c" \
  "$repo_dir/src/core/scspdsp.c" -o "$test_dir/scsp-notify-arm"
qemu-arm -cpu cortex-a9 "$test_dir/scsp-notify-arm"
