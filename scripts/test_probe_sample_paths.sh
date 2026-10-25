#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d)
trap 'rm -rf -- "$task_tmp"' EXIT
for capacity in 0 1; do
 for flags in '' '-fsanitize=address,undefined -fno-omit-frame-pointer'; do
  ${CC:-gcc} -std=c11 -O1 -g -Wall -Wextra -Werror -DX4_VIDEO_CAPACITY_1080="$capacity" \
    $flags tests/probe_sample_paths_test.c src/video/probe_contract.c src/video/video_capacity.c -o "$task_tmp/check"
  "$task_tmp/check"
 done
done
