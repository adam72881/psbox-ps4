#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
task_cc=${CC:-cc}
task_flags=${CFLAGS:-}
"$task_cc" -std=c11 -O2 -Wall -Wextra -Werror $task_flags -Isrc/media -Isrc/streaming \
    tests/test_sparse_reorder_replay.c src/media/rtp_reorder.c src/media/nack_grace.c \
    -o "$task_dir/replay"
"$task_dir/replay"
