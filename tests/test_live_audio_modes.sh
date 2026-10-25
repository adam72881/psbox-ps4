#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
audio_tmp=$(mktemp -d)
trap 'rm -f -- "$audio_tmp/test" "$audio_tmp/result.log"; rmdir -- "$audio_tmp"' EXIT
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca -pthread ${X4_TEST_CFLAGS:-} \
    -DX4_VIDEO_PROBE=0 -DX4_STREAM_PROFILE=1 -Itests/audio_stubs \
    tests/live_audio_modes_test.c -o "$audio_tmp/test"
"$audio_tmp/test" | tee "$audio_tmp/result.log"
python3 tests/live_audio_mode_source_test.py "$audio_tmp/result.log"
