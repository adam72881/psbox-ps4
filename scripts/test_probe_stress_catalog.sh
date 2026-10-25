#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/x4-stress-catalog.XXXXXX")
trap 'rm -f -- "$tmp"/*; rmdir -- "$tmp"' EXIT
for probe in 0 1;do
for capacity in 0 1;do
    "$compiler" -std=c11 -O1 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE="$probe" -DX4_VIDEO_CAPACITY_1080="$capacity" \
        "$repo/tests/probe_stress_catalog_test.c" -o "$tmp/catalog-$probe-$capacity"
    "$tmp/catalog-$probe-$capacity"
    "$compiler" -std=c11 -O1 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE="$probe" -DX4_VIDEO_CAPACITY_1080="$capacity" \
        -fsanitize=address,undefined -fno-omit-frame-pointer "$repo/tests/probe_stress_catalog_test.c" -o "$tmp/catalog-$probe-$capacity-asan"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/catalog-$probe-$capacity-asan"
done
done
