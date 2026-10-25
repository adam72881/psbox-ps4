#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Explicit host doubles: no native playback/console/crypto/timing claim.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2;exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-probe-media-mock.XXXXXX")
trap 'rm -f -- "$build_dir"/*;rmdir -- "$build_dir"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections
    -DX4_PROBE_STACK_HOST=1 -DX4_VIDEO_PROBE=1 -DX4_VIDEO_CAPACITY_1080=1 -DX4_RUMBLE=0 -DX4_RECEIVER_NACK=1 -DX4_NACK_GRACE_MS=45
    -DX4_INGRESS_HOST_TEST -DX4_GRACE_INBOX_HOST_TEST -DX4_PROBE_FILES_TESTING -I"$repo_dir/tests/host_stubs" -I"$repo_dir/src/video"
    "$repo_dir/tests/probe_media_lifecycle_test.c" "$repo_dir/src/video/probe_source.c" "$repo_dir/src/video/probe_metrics.c"
    "$repo_dir/src/media/video_ingress.c" "$repo_dir/src/media/rtp_reorder.c" "$repo_dir/src/media/nack_grace.c"
    "$repo_dir/src/media/nack_sequence_binding.c" "$repo_dir/src/streaming/nack_grace_inbox.c"
    "$repo_dir/src/video/probe_files.c" "$repo_dir/src/video/probe_stack.c")
echo "BUILD/RUN MOCK local media lifecycle"
"$compiler" "${common[@]}" -o "$build_dir/plain"
"$build_dir/plain"
if "$compiler" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$build_dir/asan" >"$build_dir/asan-build.txt" 2>&1;then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/asan"
else
    cat "$build_dir/asan-build.txt";echo "UNAVAILABLE mock ASan/UBSan build;not a sanitizer pass"
fi
if "$compiler" "${common[@]}" -fsanitize=thread -fno-omit-frame-pointer \
    -o "$build_dir/tsan" >"$build_dir/tsan-build.txt" 2>&1;then
    if TSAN_OPTIONS=halt_on_error=1 "$build_dir/tsan" >"$build_dir/tsan-run.txt" 2>&1;then
        cat "$build_dir/tsan-run.txt"
    elif grep -Eq 'FATAL: ThreadSanitizer: (unexpected memory mapping|unsupported)' "$build_dir/tsan-run.txt";then
        cat "$build_dir/tsan-run.txt";echo "UNAVAILABLE mock TSan runtime;not a race-free result"
    else
        cat "$build_dir/tsan-run.txt";echo "FAIL mock TSan runtime" >&2;exit 1
    fi
else
    cat "$build_dir/tsan-build.txt";echo "UNAVAILABLE mock TSan build;not a race-free result"
fi
