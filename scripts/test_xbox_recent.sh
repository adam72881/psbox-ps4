#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
  tests/xbox_recent_test.c src/auth/xbox_recent.c src/auth/json.c -o "$build_dir/xbox-recent-test"
"$build_dir/xbox-recent-test"
