#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
  -Itests/host_stubs tests/auth_recent_lifecycle_test.c src/auth/json.c -pthread -o "$build_dir/auth-recent-test"
"$build_dir/auth-recent-test"
