#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-probe-memory-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in plain sanitized; do
    flags=()
    if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    mkdir "$task_tmp/$variant"
    "${CC:-gcc}" -std=c11 -O2 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE=1 \
      -DX4_PROBE_FILES_TESTING -DX4_PROBE_MEMORY_TESTING "${flags[@]}" \
      tests/probe_memory_test.c src/video/probe_memory.c src/video/probe_files.c \
      -o "$task_tmp/test-$variant"
    "$task_tmp/test-$variant" "$task_tmp/$variant"
    python3 - "$task_tmp/$variant" <<'PY'
import json, pathlib, sys
reports = list(pathlib.Path(sys.argv[1]).glob('*-memory-*.json'))
assert len(reports) == 1
r = json.loads(reports[0].read_text())
assert r['schema'] == 'X4PROBE_MEMORY1' and r['cleanup_safe'] and r['result'] == 0
assert r['bytes_per_block'] == 4194304 and len(r['rows']) == 3
for row in r['rows']:
    assert row['content_verified'] and not row['allocation_retained'] and not row['mapping_retained']
    for op in row['operations']:
        assert op['completed'] == 5 and op['elapsed_us'] == [100] * 5
        assert op['MB_per_s'] == [41943.04] * 5
print('PASS memory JSON: bounded rows, samples and decimal MB/s; host rates are synthetic')
PY
done
# An inactive probe contributes no runtime code or data.
"${CC:-gcc}" -std=c11 -O2 -DX4_VIDEO_PROBE=0 -c src/video/probe_memory.c -o "$task_tmp/disabled.o"
if nm --defined-only "$task_tmp/disabled.o" | grep -q x4_probe_memory; then
    echo 'FAIL: probe memory code present with X4_VIDEO_PROBE=0' >&2; exit 1
fi
echo 'PASS inactive probe memory object has no exported probe implementation'
