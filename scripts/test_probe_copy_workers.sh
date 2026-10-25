#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-gcc};tmp=$(mktemp -d "${TMPDIR:-/tmp}/x4-probe-copy.XXXXXX")
trap 'rm -f -- "$tmp"/*;rmdir -- "$tmp"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
 -DX4_VIDEO_PROBE=0 -I"$repo/tests/video_capacity_stubs" "$repo/tests/probe_copy_workers_test.c"
 "$repo/src/video/video_capacity.c" -Wl,--gc-sections)
for san in plain address,undefined thread; do
 extra=();if [ "$san" != plain ];then extra=(-fsanitize="$san" -fno-omit-frame-pointer);fi
 "$compiler" "${common[@]}" "${extra[@]}" -o "$tmp/run"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 "$tmp/run"
done
