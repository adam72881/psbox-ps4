#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$tmp_dir"' EXIT

"${CC:-gcc}" -std=c11 -O2 -Wall -Wextra -Werror \
    src/input/touch_menu.c tests/test_touch_menu.c -o "$tmp_dir/test_touch_menu"
"$tmp_dir/test_touch_menu"
