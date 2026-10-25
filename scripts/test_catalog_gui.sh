#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
ui_test_dir=$(mktemp -d)
trap 'rm -rf -- "$ui_test_dir"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror ${X4_UI_TEST_FLAGS:-} -Itests/host_ui \
    tests/catalog_gui_capture.c src/ui/catalog_gui.c src/ui/lang.c src/ui/font.c src/ui/catalog_browser.c \
    src/ui/recent_games.c src/ui/product_metadata.c src/auth/json.c src/auth/public_image_policy.c \
    -lm -o "$ui_test_dir/gui"
mkdir -p "$2"
"$ui_test_dir/gui" "$1" "$2"
