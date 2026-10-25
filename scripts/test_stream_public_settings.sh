#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_PUBLIC=1 \
    -Itests/host_stubs src/streaming/stream_profile.c src/ui/lang.c tests/test_stream_public_settings.c \
    -o "$build_dir/public_test"
"$build_dir/public_test"

"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_PUBLIC=0 \
    -Itests/host_stubs src/streaming/stream_profile.c src/ui/lang.c tests/test_stream_public_settings.c \
    -o "$build_dir/lab_test"
"$build_dir/lab_test"
