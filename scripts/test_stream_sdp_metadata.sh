#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
for mode in plain sanitized;do
 flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests/host_stubs)
 if [[ $mode == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
 "${CC:-gcc}" "${flags[@]}" tests/stream_sdp_metadata_test.c -Wl,--gc-sections -o "$out/$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/$mode"
done
