#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc/mbedtls}
compiler=${CC:-gcc}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
test -f "$dep/library/sha256.c" || { echo "FAIL pinned MbedTLS source unavailable: $dep" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/x4-probe-ftp-stat.XXXXXX")
trap 'rm -f -- "$tmp"/*; rmdir -- "$tmp"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE=1 -DX4_PROBE_FILES_TESTING
    -I"$dep/include" "$repo/tests/probe_ftp_stat_test.c" "$repo/src/video/probe_files.c"
    "$repo/src/video/probe_source.c" "$repo/src/video/probe_reference.c" "$repo/src/video/video_capacity.c"
    "$repo/src/video/probe_hash.c" "$dep/library/sha256.c" "$dep/library/platform_util.c")
for variant in plain asan; do
    flags=(); if [[ "$variant" == asan ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    "$compiler" "${common[@]}" "${flags[@]}" -o "$tmp/$variant"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/$variant"
done
echo "PASS host FTP stat fixtures and real SHA-256, plain + ASan/UBSan; no PS4 result claimed"
