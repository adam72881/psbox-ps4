#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Integration of real path contract, actual JSON writers and production reader.
# Only the quiescent case state is synthetic; no decoder or PS4 work is run.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d /tmp/x4-pipeline-export-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
export PYTHONDONTWRITEBYTECODE=1

compile_export_test() {
    local capacity=$1 output=$2
    shift 2
    "${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections \
        -DX4_VIDEO_PROBE=1 -DX4_VIDEO_CAPACITY_1080="$capacity" -DX4_PROBE_STACK_HOST=1 \
        -DX4_PROBE_FILES_TESTING -Itests/video_capacity_stubs -Isrc/video "$@" \
        tests/probe_pipeline_export_test.c src/video/probe_files.c src/video/probe_metrics.c src/video/video_pipeline.c \
        -Wl,--gc-sections -o "$output"
}

for capacity in 0 1; do
    for variant in plain sanitized; do
        flags=()
        if [[ "$variant" == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        directory="$task_tmp/vc$capacity-$variant"
        mkdir "$directory"
        compile_export_test "$capacity" "$task_tmp/test-vc$capacity-$variant" "${flags[@]}"
        timeout 60s "$task_tmp/test-vc$capacity-$variant" "$directory" > "$task_tmp/vc$capacity-$variant.log" 2>&1 || {
            cat "$task_tmp/vc$capacity-$variant.log"; exit 1;
        }
        python3 tests/probe_pipeline_export_analysis_test.py "$directory"
        python3 - "$task_tmp/vc$capacity-$variant.log" <<'PY'
import pathlib,sys
text=pathlib.Path(sys.argv[1]).read_text()
assert text.count('XCloud4 pipeline: report=') == 54
assert 'export_failed operation=pipeline_create ' in text and ' errno=17' in text
assert text.count('export_failed operation=metrics_write_finish ') == 58
assert ' errno=28' in text
print('PASS actual exclusive-create collisions and bounded ENOSPC faults are not silent success')
PY
        echo "PASS real-export vc$capacity $variant: 54 paired reports and real reader"
    done
done

# Reproduce the old capacity mismatch in a temporary source copy. This is a
# focused regression mutant (384), not a mutation of the frozen source/ELF.
python3 - "$task_tmp/original-capacity.c" <<'PY'
import pathlib,sys
source=pathlib.Path('src/video/probe_pipeline.c').read_text()
needle='char path[X4_PROBE_FILE_PATH_MAX] = {0}, key[128];'
assert source.count(needle)==1, 'Review actual export_path implementation before updating regression test'
pathlib.Path(sys.argv[1]).write_text(source.replace(needle,'char path[384] = {0}, key[128];'))
PY
compile_export_test 1 "$task_tmp/negative" "-DX4_PIPELINE_EXPORT_SOURCE=\"$task_tmp/original-capacity.c\""
mkdir "$task_tmp/negative-output"
timeout 30s "$task_tmp/negative" "$task_tmp/negative-output" --expect-path-rejection > "$task_tmp/negative.log" 2>&1 || {
    cat "$task_tmp/negative.log"; exit 1;
}
cat "$task_tmp/negative.log"
python3 - "$task_tmp/negative.log" <<'PY'
import pathlib,sys
text=pathlib.Path(sys.argv[1]).read_text()
assert 'export_failed operation=metrics_path path_attempt=/data/xcloud4-probe-' in text
assert ' result=-1 errno=22' in text
assert 'XCloud4 pipeline: report=' not in text
print('PASS control negative: real formatter rejects capacity384 before either JSON is created')
PY
