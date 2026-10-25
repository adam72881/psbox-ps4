#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
for mode in plain sanitized; do
 flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
   -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests/host_stubs)
 if [[ $mode == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
 "${CC:-gcc}" "${flags[@]}" tests/stream_cpu_test.c src/video/stream_cpu.c src/ui/stream_settings.c \
   src/streaming/stream_profile.c -Wl,--gc-sections -o "$out/cpu-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/cpu-$mode"
 "${CC:-gcc}" "${flags[@]}" tests/stream_sdp_bandwidth_test.c src/streaming/stream_sdp_bandwidth.c \
   src/streaming/stream_profile.c -Wl,--gc-sections -o "$out/sdp-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/sdp-$mode"
done
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
for mode in plain sanitized;do
 flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
   -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests/host_stubs -I"$rtc_include")
 if [[ $mode == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
 "${CC:-gcc}" "${flags[@]}" tests/rtc_g8_offer_test.c src/streaming/stream_sdp_bandwidth.c src/streaming/stream_profile.c \
   -Wl,--gc-sections -o "$out/rtc-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/rtc-$mode"
done
