#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Configure a Vita build directory with every VITA_*/YABAUSE_* cache setting of
# an existing one, plus overrides:
#   tools/configure_like.sh build/base build/variant -DVITA_SCU_DSP_JIT=ON ...
# Reusing an existing directory is cheaper still: `cmake -S . -B DIR -DOPT=...`
# rebuilds only the objects whose flags changed.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
src=$1 dst=$2; shift 2
cache="$src/CMakeCache.txt"
[ -f "$cache" ] || { echo "no $cache" >&2; exit 1; }
args=()
while IFS= read -r line; do
  name=${line%%:*} rest=${line#*:} type=${rest%%=*} value=${rest#*=}
  args+=("-D$name:$type=$value")
done < <(grep -E '^(VITA|YABAUSE)_[A-Z0-9_]+:(BOOL|STRING|PATH|FILEPATH)=' "$cache")
toolchain=$(grep -E '^CMAKE_TOOLCHAIN_FILE:' "$cache" | cut -d= -f2-)
cmake -S "$repo_root" -B "$dst" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" "${args[@]}" "$@"
