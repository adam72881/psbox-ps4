#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-probe-pipeline-runtime-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in plain sanitized; do
    flags=()
    if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    mkdir "$task_tmp/$variant"
    "${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections \
        -DX4_VIDEO_PROBE=1 -DX4_VIDEO_CAPACITY_1080=1 -DX4_PROBE_STACK_HOST=1 \
        -Itests/video_capacity_stubs "${flags[@]}" tests/probe_pipeline_runtime_test.c \
        src/video/video_capacity.c src/video/video_pipeline.c src/video/probe_contract.c src/video/probe_stack.c \
        -Wl,--gc-sections -o "$task_tmp/test-$variant"
    timeout 60s "$task_tmp/test-$variant" "$task_tmp/$variant"
    python3 - "$task_tmp/$variant" <<'PY'
import json,pathlib,sys
rows=[json.loads(p.read_text()) for p in pathlib.Path(sys.argv[1]).glob('*-pipeline.json')]
assert rows and any(x['converters']==0 for x in rows)
assert any(x['converters']==2 and not x['retained'] for x in rows)
assert any(x['converters']==3 for x in rows)
assert all(x['output_memory_type']==0 and x['input_output_PTS_association'] is False for x in rows)
assert any(x['retained'] for x in rows)
print('PASS runtime exports parse as bounded case JSON; metrics are a host lifecycle mock')
PY
done
