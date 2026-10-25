#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_cc=${CC:-}
if [[ -z "$task_cc" ]]; then
    if command -v clang >/dev/null 2>&1; then task_cc=clang; else task_cc=gcc; fi
fi
task_tmp=$(mktemp -d /tmp/x4-stream-media-frontier-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for grace in 0 45; do
for variant in plain sanitized; do
    flags=()
    if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    "$task_cc" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections \
        -DX4_OPENORBIS -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -DX4_VIDEO_CAPACITY_1080=1 \
        -DX4_INGRESS_HOST_TEST -DX4_RUMBLE=0 -DX4_NACK_GRACE_MS="$grace" \
        -DX4_STREAM_WAIT_HOST=1 -D_POSIX_C_SOURCE=200809L -Itests/host_stubs "${flags[@]}" \
        tests/test_live_media_stream_frontier.c src/media/video_ingress.c src/video/stream_au_queue.c src/streaming/keyframe_limiter.c \
        src/video/stream_wait.c \
        -Wl,--gc-sections -o "$task_tmp/$variant"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$task_tmp/$variant"
done
done
echo 'PASS plain and ASan/UBSan actual stream media frontier'
