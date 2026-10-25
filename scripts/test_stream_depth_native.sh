#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-stream-depth-native-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
python3 tests/stream_depth_attempt_cancel_reader_test.py
for profile in 1 2; do
    for variant in plain sanitized; do
        flags=()
        if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections \
            -DX4_VIDEO_PROBE=0 -DX4_VIDEO_CAPACITY_1080=1 -DX4_STREAM_PROFILE="$profile" -DX4_PROBE_STACK_HOST=1 \
            -Itests/video_capacity_stubs "${flags[@]}" tests/stream_depth_native_test.c \
            src/video/video_capacity.c src/video/video_pipeline.c src/video/stream_sps.c \
            src/video/probe_stack.c src/video/stream_cpu.c src/video/decoder_depth.c \
            -Wl,--gc-sections -o "$task_tmp/test-$profile-$variant"
        timeout 90s "$task_tmp/test-$profile-$variant"
    done
done
