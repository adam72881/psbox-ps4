#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-probe-files-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in plain sanitized; do
    flags=()
    if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    mkdir "$task_tmp/$variant"
    "${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -Werror -DX4_PROBE_FILES_TESTING "${flags[@]}" \
      tests/probe_files_test.c src/video/probe_files.c -o "$task_tmp/test-$variant"
    "$task_tmp/test-$variant" "$task_tmp/$variant"
done
# Fail if offline root/fault mutation can be accidentally compiled for PS4.
if "${CC:-gcc}" -std=c11 -DX4_OPENORBIS -DX4_PROBE_FILES_TESTING -Itests/host_stubs \
  -c src/video/probe_files.c -o "$task_tmp/forbidden.o" 2> "$task_tmp/forbidden.log"; then
    echo "FAIL: test-only fixture accepted by a PS4 build" >&2; exit 1
fi
if ! grep -q 'Offline fixture and fault injection must never enter a PS4 build' "$task_tmp/forbidden.log"; then
    cat "$task_tmp/forbidden.log" >&2; exit 1
fi
echo "PASS native build rejects offline fixtures/fault injection"
