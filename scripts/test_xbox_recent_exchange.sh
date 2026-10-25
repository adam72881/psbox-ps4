#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
# The included production translation unit contains an existing streaming
# label followed by a declaration. That runtime section is discarded by the
# linker and is outside this UI test; do not edit it for the host harness.
compatibility_flags=""
case "$("${CC:-cc}" --version)" in
  *[Cc]lang*) compatibility_flags="-Wno-c23-extensions" ;;
esac
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
  $compatibility_flags \
  -ffunction-sections -fdata-sections -Itests/host_stubs tests/xbox_recent_exchange_test.c \
  src/auth/json.c src/auth/xbox_recent.c -Wl,--gc-sections -o "$build_dir/xbox-recent-exchange-test"
"$build_dir/xbox-recent-exchange-test"
