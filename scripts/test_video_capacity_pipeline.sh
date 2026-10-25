#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-video-pipeline.XXXXXX")
trap 'rm -f -- "$build_dir"/*; rmdir -- "$build_dir"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections
    -I"$repo_dir/tests/video_capacity_stubs" -I"$repo_dir/src/video"
    "$repo_dir/tests/video_capacity_pipeline_test.c" "$repo_dir/src/video/video_capacity.c")
for flag in 0 1; do
    "$compiler" "${common[@]}" -DX4_VIDEO_CAPACITY_1080="$flag" -o "$build_dir/$flag-plain"
    "$build_dir/$flag-plain"
    "$compiler" "${common[@]}" -DX4_VIDEO_CAPACITY_1080="$flag" -fsanitize=address,undefined -fno-omit-frame-pointer -o "$build_dir/$flag-asan"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/$flag-asan"
done
