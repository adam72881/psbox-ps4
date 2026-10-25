#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc/mbedtls}
compiler=${CC:-gcc}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/x4-probe-hash.XXXXXX")
trap 'rm -f -- "$tmp"/*; rmdir -- "$tmp"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE=1
 -I"$dep/include" "$repo/tests/probe_hash_test.c" "$repo/src/video/probe_hash.c"
 "$dep/library/sha256.c" "$dep/library/platform_util.c")
for san in plain asan; do
 extra=();if [ "$san" = asan ];then extra=(-fsanitize=address,undefined -fno-omit-frame-pointer);fi
 "$compiler" "${common[@]}" "${extra[@]}" -o "$tmp/$san"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/$san"
done
