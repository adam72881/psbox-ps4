#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
compatibility_flags=""
case "$("${CC:-cc}" --version)" in
  *[Cc]lang*) compatibility_flags="-Wno-c23-extensions" ;;
esac
for profile in 0 1 2; do
 for mode in plain sanitized; do
    sanitizer_flags=""
    if [[ $mode == sanitized ]]; then sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie"; fi
  "${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
    $compatibility_flags -DX4_STREAM_PROFILE="$profile" \
    -ffunction-sections -fdata-sections -Itests/host_stubs \
    $sanitizer_flags tests/stream_os_profile_http_test.c src/auth/http_client.c src/auth/json.c src/auth/xbox_recent.c src/streaming/stream_profile.c \
    -Wl,--gc-sections -o "$build_dir/stream-os-profile-$profile-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/stream-os-profile-$profile-$mode"
 done
done
