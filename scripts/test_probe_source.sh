#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-probe-source.XXXXXX")
trap 'rm -f -- "$build_dir"/*; rmdir -- "$build_dir"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -DX4_PROBE_FILES_TESTING -I"$repo_dir/src/video"
    "$repo_dir/tests/probe_source_test.c" "$repo_dir/src/video/probe_source.c" "$repo_dir/src/video/probe_files.c")
for flag in 0 1; do
    echo "BUILD/RUN local probe source flag=$flag"
    "$compiler" "${common[@]}" -DX4_VIDEO_PROBE="$flag" -o "$build_dir/$flag-plain"
    "$build_dir/$flag-plain"
    if "$compiler" "${common[@]}" -DX4_VIDEO_PROBE="$flag" -fsanitize=address,undefined -fno-omit-frame-pointer \
        -o "$build_dir/$flag-asan" >"$build_dir/$flag-asan-build.txt" 2>&1; then
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/$flag-asan"
    else
        cat "$build_dir/$flag-asan-build.txt"
        echo "UNAVAILABLE probe source flag=$flag ASan/UBSan build; not a sanitizer pass"
    fi
done
if "$compiler" "${common[@]}" -DX4_VIDEO_PROBE=2 -o "$build_dir/invalid" >"$build_dir/invalid-build.txt" 2>&1; then
    echo "FAIL invalid probe flag compiled" >&2; exit 1
fi
grep -q 'X4_VIDEO_PROBE must be 0 or 1' "$build_dir/invalid-build.txt" || {
    cat "$build_dir/invalid-build.txt"; echo "FAIL invalid flag failed for unrelated reason" >&2; exit 1;
}
echo "PASS invalid probe flag rejected"
