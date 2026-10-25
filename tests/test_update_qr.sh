#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/tests"
mkdir -p "${BUILD_DIR}"

[ -f "$HOME/.config/xcloud4/env.sh" ] && source "$HOME/.config/xcloud4/env.sh"
OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}"

"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -pthread \
    -isystem "${OO_PS4_TOOLCHAIN}/include" \
    -I"${REPO_ROOT}" \
    -I"${REPO_ROOT}/src" \
    -I"${REPO_ROOT}/third_party" \
    -I"${REPO_ROOT}/tests/host_stubs" \
    "${REPO_ROOT}/tests/test_update_qr.c" \
    "${REPO_ROOT}/src/auth/release_check.c" \
    "${REPO_ROOT}/src/auth/json.c" \
    "${REPO_ROOT}/src/ui/screen.c" \
    "${REPO_ROOT}/src/ui/canvas.c" \
    "${REPO_ROOT}/src/ui/font.c" \
    "${REPO_ROOT}/src/ui/lang.c" \
    "${REPO_ROOT}/src/input/touch_menu.c" \
    "${REPO_ROOT}/third_party/qrcodegen/qrcodegen.c" \
    -o "${BUILD_DIR}/test_update_qr"

"${BUILD_DIR}/test_update_qr"
echo "PASS: test_update_qr.sh"
