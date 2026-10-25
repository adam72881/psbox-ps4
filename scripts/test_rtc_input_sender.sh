#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
for mode in plain sanitized; do
 flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_PROFILE=1 -DX4_RUMBLE=1
  -ffunction-sections -fdata-sections -Itests/host_stubs -I"$rtc_include")
 if [[ $mode == sanitized ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
 "${CC:-gcc}" "${flags[@]}" tests/rtc_input_sender_test.c src/streaming/stream_profile.c src/input/rumble.c \
  -Wl,--gc-sections -o "$out/input-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/input-$mode"
done
