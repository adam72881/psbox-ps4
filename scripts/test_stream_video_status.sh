#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for profile in 1 2; do
  "${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
    -DX4_STREAM_PROFILE="$profile" tests/stream_video_status_test.c src/ui/font.c \
    -o "$build_dir/stream-video-status-$profile"
  "$build_dir/stream-video-status-$profile" assets/fonts/noto_sans.x4font
done
