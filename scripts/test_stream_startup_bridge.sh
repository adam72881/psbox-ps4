#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
"${PYTHON:-python3}" -B tests/stream_startup_bridge_source_test.py
bash scripts/test_stream_startup_auth_bridge.sh
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
compatibility_flags=""
case "$("${CC:-cc}" --version)" in
  *[Cc]lang*) compatibility_flags="-Wno-c23-extensions" ;;
esac
for mode in plain sanitized; do
  sanitizer_flags=""
  if [[ $mode == sanitized ]]; then sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie"; fi
  "${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
    $compatibility_flags -DX4_STREAM_PROFILE=1 -ffunction-sections -fdata-sections -Itests/host_stubs \
    $sanitizer_flags tests/stream_startup_xbox_bridge_test.c src/auth/http_client.c src/auth/json.c \
    src/auth/xbox_recent.c src/streaming/stream_profile.c src/streaming/keyframe_limiter.c -Wl,--gc-sections \
    -o "$build_dir/startup-xbox-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/startup-xbox-$mode"
done
