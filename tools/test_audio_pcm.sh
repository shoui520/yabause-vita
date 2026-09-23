#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f -- "$test_dir/audio" "$test_dir/queue"; rmdir -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_audio_pcm.c" -o "$test_dir/audio"
"$test_dir/audio"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_dir/tests/test_audio_output_queue.c" "$repo_dir/src/vita/audio_output_queue.c" -o "$test_dir/queue"
"$test_dir/queue"
if [[ "${AUDIO_QUEUE_TSAN:-0}" == 1 ]]; then
  # Non-PIE has run successfully here; host ASLR can still prevent TSan startup.
  "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
    -fsanitize=thread -fno-pie -no-pie \
    "$repo_dir/tests/test_audio_output_queue.c" "$repo_dir/src/vita/audio_output_queue.c" -o "$test_dir/queue"
  "$test_dir/queue"
fi
