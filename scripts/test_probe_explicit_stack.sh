#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Native attribute API mapped to actual POSIX pthreads: no PS4 evidence.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-probe-explicit-stack.XXXXXX")
trap 'rm -f -- "$build_dir"/*;rmdir -- "$build_dir"' EXIT
common=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread -DX4_VIDEO_PROBE=1 -DX4_PROBE_STACK_HOST=1 -DX4_PROBE_STACK_TEST_WAIT_LIMIT=500
    -I"$repo_dir/tests/host_stubs" "$repo_dir/tests/probe_explicit_stack_test.c" "$repo_dir/src/video/probe_stack.c")
"$compiler" "${common[@]}" -o "$build_dir/plain"
"$build_dir/plain"
"$compiler" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer -o "$build_dir/asan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=0 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/asan"

