#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${X4_STARTUP_TEST_OUTPUT:-$(mktemp -d)}
if [[ -z ${X4_STARTUP_TEST_OUTPUT:-} ]];then trap 'rm -rf -- "$out"' EXIT;fi
mkdir -p "$out"
for profile in 1 2;do
 for mode in plain sanitized;do
  destination="$out/profile-$profile-$mode";mkdir -p "$destination"
  flags=(-std=gnu2x -O1 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections
   -DX4_VIDEO_PROBE=0 -DX4_VIDEO_CAPACITY_1080=1 -DX4_STREAM_PROFILE="$profile" -DX4_OPENORBIS=1 -DX4_STREAM_WAIT_HOST=1
   -Itests/video_capacity_stubs -Itests/host_stubs)
  if [[ $mode == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
  "${CC:-gcc}" "${flags[@]}" tests/stream_startup_test.c src/streaming/stream_startup.c -o "$destination/policy"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$destination/policy"
  "${CC:-gcc}" "${flags[@]}" tests/stream_startup_integration_test.c \
   src/video/video_capacity.c src/video/video_pipeline.c src/video/decoder_depth.c src/video/stream_cpu.c src/video/stream_wait.c src/video/stream_au_queue.c src/video/stream_sps.c src/video/stream_diagnostics.c src/video/stream_color_policy.c \
   src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c src/media/live_trace.c src/media/live_monitor.c src/media/video_ingress.c \
   -Wl,--gc-sections -o "$destination/integration"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 180s "$destination/integration" "$destination"
  for report in "$destination"/xcloud4-stream-*-p*-*.json;do
   "${PYTHON:-python3}" -B scripts/analyze_stream_pipeline.py "$report" > /dev/null
  done
 done
done
printf 'PASS initial resolution unit/real parser/scheduler/native-double integration, profiles1/2, plain+ASan/UBSan; ten seconds are a controlled host clock.\n'
