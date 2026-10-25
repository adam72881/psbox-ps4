#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
"${PYTHON:-python3}" -B tests/keyframe_wiring_source_test.py
bash scripts/test_stream_startup_bridge.sh
out=${X4_KEYFRAME_WIRING_TEST_OUTPUT:-$(mktemp -d)}
if [[ -z ${X4_KEYFRAME_WIRING_TEST_OUTPUT:-} ]]; then trap 'rm -rf -- "$out"' EXIT; fi
mkdir -p "$out"
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
for profile in 0 1 2; do
    for mode in plain sanitized; do
        flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_PROFILE="$profile"
            -ffunction-sections -fdata-sections -Itests/host_stubs -I"$rtc_include")
        if [[ $mode == sanitized ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
        "${CC:-gcc}" "${flags[@]}" tests/keyframe_wiring_test.c src/streaming/stream_profile.c \
            src/streaming/stream_startup.c src/streaming/keyframe_limiter.c src/auth/json.c \
            -Wl,--gc-sections -o "$out/wiring-$profile-$mode"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            timeout 180s "$out/wiring-$profile-$mode" | tee "$out/wiring-$profile-$mode.log"
    done
done
