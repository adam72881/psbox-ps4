#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "UNAVAILABLE compiler=$compiler" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/x4-probe-contract.XXXXXX")
cleanup() { rm -f -- "$tmp"/*; rmdir -- "$tmp"; }
trap cleanup EXIT
sources=("$repo/tests/probe_contract_test.c" "$repo/src/video/probe_contract.c" "$repo/src/video/video_capacity.c")
common=(-std=c11 -O1 -g -Wall -Wextra -Werror)
"$compiler" --version
for capacity in 0 1; do
    "$compiler" "${common[@]}" "${sources[@]}" -DX4_VIDEO_CAPACITY_1080="$capacity" -o "$tmp/$capacity"
    "$tmp/$capacity"
    if "$compiler" "${common[@]}" "${sources[@]}" -DX4_VIDEO_CAPACITY_1080="$capacity" \
        -fsanitize=address,undefined -fno-omit-frame-pointer -o "$tmp/$capacity-sanitized" \
        >"$tmp/$capacity-build.log" 2>&1; then
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/$capacity-sanitized"
    else
        cat "$tmp/$capacity-build.log"
        echo "UNAVAILABLE ASan/UBSan capacity=$capacity (not a sanitizer pass)"
    fi
done
