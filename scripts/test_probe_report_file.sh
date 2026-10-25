#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-probe-report-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for flags in '' '-fsanitize=address,undefined -fno-omit-frame-pointer'; do
 ${CC:-gcc} -std=c11 -O1 -g -Wall -Wextra -Werror -DX4_PROBE_FILES_TESTING $flags tests/probe_report_file_test.c src/video/probe_files.c -o "$task_tmp/check"
 "$task_tmp/check" "$task_tmp"
done
