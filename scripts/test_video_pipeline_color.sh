#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-video-color-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in sse2 scalar sanitized sanitized-scalar; do
    flags=()
    if [[ "$variant" == *scalar ]]; then flags+=(-DX4_PIPELINE_SCALAR=1); fi
    if [[ "$variant" == sanitized* ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    "${CC:-gcc}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
        "${flags[@]}" tests/video_pipeline_color_test.c src/video/video_pipeline.c \
        -lm -o "$task_tmp/$variant"
    "$task_tmp/$variant" | tee "$task_tmp/$variant.txt"
    grep '^PASS color checks=' "$task_tmp/$variant.txt" > "$task_tmp/$variant.hash"
    if [[ "$variant" != sse2 ]]; then cmp "$task_tmp/sse2.hash" "$task_tmp/$variant.hash"; fi
done
echo 'PASS scalar/SSE2 full-frame hashes identical, with ASan/UBSan in both paths'
