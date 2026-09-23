#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/host" "$test_dir/arm" "$test_dir/owners" "$test_dir/runtime"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_telemetry_sampling.c" -o "$test_dir/host"
"$test_dir/host"
"${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=c11 -O2 -static -mcpu=cortex-a9 \
  -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  "$repo_dir/tests/test_telemetry_sampling.c" -o "$test_dir/arm"
qemu-arm -cpu cortex-a9 "$test_dir/arm"
"${CC:-cc}" -std=c11 -O2 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_telemetry_owners.c" -o "$test_dir/owners"
"$test_dir/owners"
"${CC:-cc}" -std=gnu11 -O2 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$repo_dir/tests/telemetry_stubs" "$repo_dir/tests/test_telemetry_runtime.c" \
  "$repo_dir/src/vita/telemetry.c" -o "$test_dir/runtime"
"$test_dir/runtime"
