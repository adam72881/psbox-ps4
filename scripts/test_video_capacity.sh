#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Explicitly requested offline preparation; no PS4/decoder/network execution.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-video-capacity.XXXXXX")
cleanup() { rm -f -- "$build_dir"/*; rmdir -- "$build_dir"; }
trap cleanup EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -I"$repo_dir/src/video"
    "$repo_dir/tests/video_capacity_test.c" "$repo_dir/src/video/video_capacity.c")
echo "Host video capacity compiler: $compiler"
"$compiler" --version
for flag in 0 1; do
    echo "BUILD/RUN capacity flag=$flag"
    "$compiler" "${common[@]}" -DX4_VIDEO_CAPACITY_1080="$flag" -o "$build_dir/$flag-plain"
    "$build_dir/$flag-plain"
    if "$compiler" "${common[@]}" -DX4_VIDEO_CAPACITY_1080="$flag" \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -o "$build_dir/$flag-asan" >"$build_dir/$flag-asan-build.txt" 2>&1; then
        echo "RUN capacity flag=$flag ASan + UBSan"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/$flag-asan"
    else
        cat "$build_dir/$flag-asan-build.txt"
        echo "UNAVAILABLE capacity flag=$flag ASan/UBSan build; not a sanitizer pass"
    fi
done
echo "CHECK invalid capacity flag rejected at compile time"
if "$compiler" "${common[@]}" -DX4_VIDEO_CAPACITY_1080=2 -o "$build_dir/invalid" \
    >"$build_dir/invalid-build.txt" 2>&1; then
    echo "FAIL invalid flag compiled" >&2
    exit 1
fi
grep -q 'X4_VIDEO_CAPACITY_1080 must be 0 or 1' "$build_dir/invalid-build.txt" || {
    cat "$build_dir/invalid-build.txt"; echo "FAIL invalid flag failed for an unrelated reason" >&2; exit 1;
}
echo "PASS invalid capacity flag rejected; no native performance claim"
