#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -n ${CC:-} ]]; then compiler=$CC
elif command -v gcc >/dev/null 2>&1; then compiler=gcc
elif command -v clang >/dev/null 2>&1; then compiler=clang
else printf 'No host C compiler available\n' >&2; exit 1
fi
out=${X4_KEYFRAME_TEST_OUTPUT:-$(mktemp -d)}
if [[ -z ${X4_KEYFRAME_TEST_OUTPUT:-} ]]; then trap 'rm -rf -- "$out"' EXIT; fi
mkdir -p "$out"
for mode in plain sanitized; do
    flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread)
    if [[ $mode == sanitized ]]; then
        flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
    fi
    "$compiler" "${flags[@]}" tests/test_keyframe_limiter.c src/streaming/keyframe_limiter.c -o "$out/$mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        timeout 180s "$out/$mode" | tee "$out/$mode.log"
done
