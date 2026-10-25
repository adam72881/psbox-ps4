#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
# Use the real libdatachannel public C declarations, exactly as the native
# build does. Only the function implementations at the channel boundary are
# host simulations. An absent dependency is a failure, never a skipped test.
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
for profile in 0 1 2; do
 for mode in plain sanitized; do
  flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_PROFILE="$profile"
   -ffunction-sections -fdata-sections -Itests/host_stubs -I"$rtc_include")
  if [[ $mode == sanitized ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
  "${CC:-gcc}" "${flags[@]}" tests/rtc_resolution_startup_test.c src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c src/auth/json.c \
   -Wl,--gc-sections -o "$out/startup-$profile-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/startup-$profile-$mode"
 done
done
