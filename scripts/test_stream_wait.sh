#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_output=${X4_STREAM_WAIT_TEST_OUTPUT:-$(mktemp -d /tmp/x4-stream-wait-XXXXXX)}
if [[ -z ${X4_STREAM_WAIT_TEST_OUTPUT:-} ]];then trap 'rm -rf -- "$task_output"' EXIT;fi
mkdir -p "$task_output"
for variant in plain sanitized;do
 flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_WAIT_HOST=1 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0)
 if [[ $variant == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
 "${CC:-gcc}" "${flags[@]}" tests/stream_wait_test.c src/video/stream_wait.c -o "$task_output/$variant"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s "$task_output/$variant"
done
echo 'PASS production wait core with POSIX semaphore adapter, plain + ASan/UBSan'
