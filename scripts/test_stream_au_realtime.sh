#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_cc=${CC:-}
if [[ -z "$task_cc" ]]; then
    if command -v clang >/dev/null 2>&1; then task_cc=clang; else task_cc=gcc; fi
fi
task_out=${X4_AU_REALTIME_TEST_OUTPUT:-$(mktemp -d /tmp/x4-au-realtime-XXXXXX)}
if [[ -z ${X4_AU_REALTIME_TEST_OUTPUT:-} ]]; then trap 'rm -rf -- "$task_out"' EXIT; fi
mkdir -p "$task_out"
implementations=(current)
if [[ -n ${X4_AU_REALTIME_COMPARE_SOURCE:-} ]]; then
    : "${X4_AU_REALTIME_COMPARE_SHA256:?External source requires an exact SHA-256}"
    : "${X4_AU_REALTIME_COMPARE_HEADER:?External source requires its paired header}"
    : "${X4_AU_REALTIME_COMPARE_HEADER_SHA256:?External header requires an exact SHA-256}"
    [[ $X4_AU_REALTIME_COMPARE_SHA256 =~ ^[0-9a-f]{64}$ ]]
    [[ $X4_AU_REALTIME_COMPARE_HEADER_SHA256 =~ ^[0-9a-f]{64}$ ]]
    test "$(sha256sum "$X4_AU_REALTIME_COMPARE_SOURCE" | cut -d' ' -f1)" = "$X4_AU_REALTIME_COMPARE_SHA256"
    test "$(sha256sum "$X4_AU_REALTIME_COMPARE_HEADER" | cut -d' ' -f1)" = "$X4_AU_REALTIME_COMPARE_HEADER_SHA256"
    # A source comparison is allowed only with byte-identical public ABI.
    cmp -s "$X4_AU_REALTIME_COMPARE_HEADER" src/video/stream_au_queue.h
    mkdir -p "$task_out/before_preflight"
    cp -- "$X4_AU_REALTIME_COMPARE_SOURCE" "$task_out/before_preflight/stream_au_queue.c"
    cp -- "$X4_AU_REALTIME_COMPARE_HEADER" "$task_out/before_preflight/stream_au_queue.h"
    test "$(sha256sum "$task_out/before_preflight/stream_au_queue.c" | cut -d' ' -f1)" = "$X4_AU_REALTIME_COMPARE_SHA256"
    test "$(sha256sum "$task_out/before_preflight/stream_au_queue.h" | cut -d' ' -f1)" = "$X4_AU_REALTIME_COMPARE_HEADER_SHA256"
    implementations+=(before_preflight)
fi
sha256sum tests/test_stream_au_realtime.c src/video/stream_au_queue.c src/video/stream_au_queue.h > "$task_out/current-inputs.sha256"
for implementation in "${implementations[@]}"; do
    task_source=src/video/stream_au_queue.c
    if [[ $implementation == before_preflight ]]; then task_source="$task_out/before_preflight/stream_au_queue.c"; fi
    for variant in plain sanitized; do
        flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
        if [[ $variant == sanitized ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
        "$task_cc" "${flags[@]}" tests/test_stream_au_realtime.c "$task_source" -o "$task_out/$implementation-$variant"
        sha256sum --check --status "$task_out/current-inputs.sha256"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            timeout 45s "$task_out/$implementation-$variant" "$implementation" | tee "$task_out/$implementation-$variant.json"
    done
done
printf 'PASS real-pthread AU workload: plain + ASan/UBSan; timing results are host observations, not PS4 claims.\n'
