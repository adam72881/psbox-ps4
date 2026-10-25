#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-stream-display-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for profile in 1 2; do
    for variant in plain sanitized; do
        flags=()
        if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -ffunction-sections -fdata-sections \
            -DX4_VIDEO_PROBE=0 -DX4_STREAM_PROFILE="$profile" -Itests/video_capacity_stubs \
            "${flags[@]}" tests/stream_display_test.c -Wl,--gc-sections -o "$task_tmp/test-$profile-$variant"
        timeout 60s "$task_tmp/test-$profile-$variant"
    done
done
