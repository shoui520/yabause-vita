#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/telemetry" "$test_dir/telemetry-cpp" "$test_dir/disc" "$test_dir/scope-c" "$test_dir/scope-cpp" "$test_dir/scope-arm" "$test_dir/scope-arm-cpp"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_telemetry_accounting.c" -o "$test_dir/telemetry"
"$test_dir/telemetry"
"${CXX:-c++}" -x c++ -std=c++17 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_telemetry_accounting.c" -o "$test_dir/telemetry-cpp"
"$test_dir/telemetry-cpp"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_disc_telemetry.c" -o "$test_dir/disc"
"$test_dir/disc"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_telemetry_scope.c" -o "$test_dir/scope-c"
"$test_dir/scope-c"
"${CXX:-c++}" -x c++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_telemetry_scope.c" -o "$test_dir/scope-cpp"
"$test_dir/scope-cpp"
"${ARM_CC:-arm-linux-gnueabihf-gcc}" -std=c11 -O2 -static -mcpu=cortex-a9 \
  -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  "$repo_dir/tests/test_telemetry_scope.c" -o "$test_dir/scope-arm"
qemu-arm -cpu cortex-a9 "$test_dir/scope-arm"
"${ARM_CXX:-arm-linux-gnueabihf-g++}" -x c++ -std=c++17 -O2 -static -mcpu=cortex-a9 \
  -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Werror \
  "$repo_dir/tests/test_telemetry_scope.c" -o "$test_dir/scope-arm-cpp"
qemu-arm -cpu cortex-a9 "$test_dir/scope-arm-cpp"
