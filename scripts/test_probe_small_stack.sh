#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc/mbedtls}
compiler=${CC:-gcc}
# Do not silently substitute the host default or claim PS4 equivalence.
bytes=${X4_PROBE_TEST_STACK_BYTES:?Set the POSIX reference stack bytes explicitly}
origin=${X4_PROBE_TEST_STACK_ORIGIN:?Set host-reference or native-evidence explicitly}
[[ "$bytes" =~ ^[0-9]+$ ]] || { echo 'FAIL nonnumeric stack size' >&2; exit 2; }
[[ "$origin" = host-reference || "$origin" = native-evidence ]] || { echo 'FAIL unknown stack evidence origin' >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/x4-probe-small-stack.XXXXXX")
trap 'rm -f -- "$tmp"/*; rmdir -- "$tmp"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -DX4_VIDEO_PROBE=1
 -DX4_PROBE_FILES_TESTING -DX4_TEST_STACK_BYTES="$bytes" -DX4_TEST_STACK_ORIGIN=\""$origin"\" -I"$dep/include" -I"$repo/src/video"
 "$repo/tests/probe_small_stack_test.c" "$repo/src/video/probe_hash.c"
 "$repo/src/video/probe_source.c" "$repo/src/video/probe_files.c"
 "$repo/src/video/probe_metrics.c"
 "$dep/library/sha256.c" "$dep/library/platform_util.c")
for san in plain asan; do
 extra=(); if [ "$san" = asan ]; then extra=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
 "$compiler" "${common[@]}" "${extra[@]}" -o "$tmp/$san"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/$san"
done
