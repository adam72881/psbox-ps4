#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
font_test_dir="$(mktemp -d)"
trap 'rm -rf -- "$font_test_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${X4_FONT_TEST_FLAGS:-} \
    tests/font_test.c src/ui/font.c -o "$font_test_dir/font_test"
"$font_test_dir/font_test" assets/fonts/noto_sans.x4font "${1:-$font_test_dir/font.ppm}" "$font_test_dir/invalid.x4font"
