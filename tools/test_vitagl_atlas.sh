#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/atlas" "$test_dir/extent"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/tests/test_atlas_extent.c" -o "$test_dir/extent"
"$test_dir/extent"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -DVITA_ATLAS_REPLACE -DVITA_ATLAS_TRIM -DVITA_ATLAS_PRIVATE_TRIM -DVITA_ATLAS_CONTENT_REUSE \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vitagl_atlas.c" -o "$test_dir/atlas"
"$test_dir/atlas"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -DVITA_ATLAS_REPLACE -DVITA_ATLAS_TRIM -DVITA_ATLAS_CONTENT_REUSE \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vitagl_atlas.c" -o "$test_dir/atlas"
"$test_dir/atlas"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -DVITA_ATLAS_REPLACE -DVITA_ATLAS_TRIM -DVITA_ATLAS_PRIVATE_TRIM \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vitagl_atlas.c" -o "$test_dir/atlas"
"$test_dir/atlas"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vitagl_atlas.c" -o "$test_dir/atlas"
"$test_dir/atlas"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -DVITA_ATLAS_REPLACE -DVITA_ATLAS_TRIM \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vitagl_atlas.c" -o "$test_dir/atlas"
"$test_dir/atlas"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -DVITA_ATLAS_REPLACE \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_vitagl_atlas.c" -o "$test_dir/atlas"
"$test_dir/atlas"
