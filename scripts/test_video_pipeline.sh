#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-video-pipeline-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in plain scalar shared sanitized; do
    flags=()
    probe=1
    if [[ "$variant" == shared ]]; then probe=0; fi
    if [[ "$variant" == scalar ]]; then flags=(-DX4_PIPELINE_SCALAR=1); fi
    if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    "${CC:-gcc}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -DX4_VIDEO_PROBE=$probe \
        "${flags[@]}" tests/video_pipeline_test.c src/video/video_pipeline.c -o "$task_tmp/test-$variant"
    "$task_tmp/test-$variant"
    "${CC:-gcc}" -std=c11 -O2 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE=$probe \
        "${flags[@]}" tests/video_pipeline_oneflight_test.c src/video/video_pipeline.c -o "$task_tmp/model-$variant"
    "$task_tmp/model-$variant"
done
echo 'PASS shared module exercised with X4_VIDEO_PROBE=0 and =1'
