#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_cc=${CC:-}
if [[ -z "$task_cc" ]]; then
    if command -v clang >/dev/null 2>&1; then task_cc=clang; else task_cc=gcc; fi
fi
task_tmp=$(mktemp -d /tmp/x4-stream-au-queue-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in plain sanitized; do
    flags=()
    if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    "$task_cc" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_AU_QUEUE_TEST_HOOKS \
        "${flags[@]}" tests/test_stream_au_queue.c src/video/stream_au_queue.c -o "$task_tmp/$variant"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$task_tmp/$variant"
done
echo 'PASS plain and ASan/UBSan stream AU queue'
