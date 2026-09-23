#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/coefficient" "$test_dir/row" "$test_dir/gpu"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_rotation_coefficient.c" -o "$test_dir/coefficient"
"$test_dir/coefficient"
"${CC:-cc}" -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_rotation_row.c" -o "$test_dir/row"
"$test_dir/row"
"${CC:-cc}" -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_rotation_gpu_queue.c" -o "$test_dir/gpu"
"$test_dir/gpu"
"${CC:-cc}" -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_rotation_map_cache.c" -o "$test_dir/gpu"
"$test_dir/gpu"
"${CC:-cc}" -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_rotation_vram_reuse.c" -o "$test_dir/gpu"
"$test_dir/gpu"
"${CC:-cc}" -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_rotation_target.c" -o "$test_dir/gpu"
"$test_dir/gpu"
if command -v arm-linux-gnueabihf-gcc >/dev/null && command -v qemu-arm >/dev/null; then
  arm-linux-gnueabihf-gcc -O3 -mcpu=cortex-a9 -mfpu=neon -static -Wall -Wextra -Werror \
    "$repo_dir/tests/test_rotation_vram_reuse.c" -o "$test_dir/gpu"
  qemu-arm -cpu cortex-a9 "$test_dir/gpu"
  arm-linux-gnueabihf-gcc -O3 -mcpu=cortex-a9 -mfpu=neon -static -Wall -Wextra -Werror \
    "$repo_dir/tests/test_rotation_map_cache.c" -o "$test_dir/gpu"
  qemu-arm -cpu cortex-a9 "$test_dir/gpu"
fi
