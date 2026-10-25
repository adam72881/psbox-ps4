#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Scheduler unit test with raw-adapter substitutes; real raw integration is
# test_stream_resilience.sh. Neither fixture measures PS4 performance.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-gcc}
base=${X4_STREAM_TEST_OUTPUT:-$(mktemp -d)}
if [[ -z "${X4_STREAM_TEST_OUTPUT:-}" ]]; then trap 'rm -rf "$base"' EXIT; fi
mkdir -p "$base"
cd "$root"
for profile in 1 2; do
 for mode in plain sanitized; do
  out="$base/profile-$profile-$mode";mkdir -p "$out"
  flags=(-std=gnu2x -O1 -g -Wall -Wextra -pthread -DX4_OPENORBIS=1 -DX4_STREAM_WAIT_HOST=1 -DX4_STREAM_PIPELINE_TEST_HOOKS=1 -DX4_STREAM_PROFILE="$profile" -DX4_VIDEO_PROBE=0 -Itests/video_capacity_stubs -Itests/host_stubs)
  if [[ "$mode" == sanitized ]];then flags+=(-fno-omit-frame-pointer -fsanitize=address,undefined -no-pie);fi
  "$cc" "${flags[@]}" tests/stream_pipeline_runtime_test.c src/video/video_pipeline.c src/video/stream_cpu.c src/video/stream_wait.c src/video/stream_au_queue.c src/video/stream_diagnostics.c src/video/stream_sps.c src/video/stream_color_policy.c src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c -o "$out/runtime"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s "$out/runtime" "$out"
  python3 tests/stream_main_polling_analysis_test.py "$out/main-polling-measurements.json"
  python3 tests/stream_pipeline_export_test.py "$out" "$profile"
  python3 tests/stream_cpu_pipeline_export_test.py "$out"
  python3 tests/stream_report_reader_test.py "$out"/xcloud4-stream-*-p*-11.json
  "$cc" "${flags[@]}" tests/stream_diagnostics_test.c src/video/stream_diagnostics.c -o "$out/diagnostics"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/diagnostics" "$out/diagnostics.json"
  python3 tests/stream_diagnostics_analysis_test.py "$out/diagnostics.json"
 done
done
printf 'PASS streaming scheduler/export/diagnostics profiles 1/2, plain and ASan/UBSan; native operations are mocks.\n'
