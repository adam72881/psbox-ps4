#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Real C sparse storage and frozen0.7.37 audio differential. Offline only.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-clang}; temp=$(mktemp -d)
trap 'rm -f -- "$temp"/*; rmdir -- "$temp"' EXIT
oracle="$repo/tests/fixtures/rtp_reorder_0737_reference.c"
expected=edb4b9869329ce38a67cefd685b7755f2fd96aedbbbc6821412e4a8a7478b5a7
actual=$(sha256sum -- "$oracle"); actual=${actual%% *}
[[ "$actual" == "$expected" ]] || { echo 'FAIL frozen audio reference SHA256' >&2; exit 1; }
echo "ORACLE exact frozen0.7.37 SHA256=$actual"
args=(-std=c11 -O1 -g -Wall -Wextra -Werror -I"$repo/src/media"
    "$repo/src/media/rtp_reorder.c" "$repo/tests/test_rtp_sparse.c")
"$cc" --version
"$cc" "${args[@]}" -o "$temp/test"; "$temp/test"
if "$cc" "${args[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$temp/san" >"$temp/san-build.txt" 2>&1; then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$temp/san"
else
    cat "$temp/san-build.txt"
    echo 'UNAVAILABLE ASan/UBSan build; not a sanitizer pass'
fi
