#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Same production writer + controlled-clock harness, probe-only scratch storage.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-cc}
out=${X4_MONITOR_STACK_TEST_OUTPUT:-}
if [[ -z "$out" ]]; then
    echo 'Set X4_MONITOR_STACK_TEST_OUTPUT to a fresh evidence directory' >&2
    exit 2
fi
mkdir -- "$out"
out=$(cd -- "$out" && pwd)
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread
    -DX4_MONITOR_HOST_TEST -DX4_MONITOR_TEST_HOOKS -DX4_INGRESS_HOST_TEST -DX4_INGRESS_TEST_HOOKS
    -I"$repo/tests/host_stubs" -I"$repo/src/media"
    "$repo/tests/progress_monitor_test.c" "$repo/src/media/live_monitor.c" "$repo/src/media/video_ingress.c")
for variant in 0 1; do
    mkdir -- "$out/probe$variant"
    "$compiler" "${common[@]}" -DX4_VIDEO_PROBE="$variant" -o "$out/probe$variant-test"
    X4_MONITOR_STACK_EQUIVALENCE_ONLY=1 X4_MONITOR_EXPORT_FIXTURES="$out/probe$variant" \
        "$out/probe$variant-test" >"$out/probe$variant.log" 2>&1
    cat "$out/probe$variant.log"
done
mkdir -- "$out/probe1-sanitized"
"$compiler" "${common[@]}" -DX4_VIDEO_PROBE=1 -fsanitize=address,undefined -fno-omit-frame-pointer -o "$out/probe1-asan-ubsan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    X4_MONITOR_STACK_EQUIVALENCE_ONLY=1 X4_MONITOR_EXPORT_FIXTURES="$out/probe1-sanitized" \
    "$out/probe1-asan-ubsan" >"$out/probe1-sanitized.log" 2>&1
cat "$out/probe1-sanitized.log"
python3 - "$out" <<'PY'
import hashlib, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
files = sorted((out / 'probe0').glob('*.bin'))
assert len(files) == 4
receipts = []
for base in files:
    expected = base.read_bytes()
    for variant in ('probe1', 'probe1-sanitized'):
        actual = (out / variant / base.name).read_bytes()
        assert actual == expected, (variant, base.name)
    receipts.append(dict(file=base.name, bytes=len(expected), sha256=hashlib.sha256(expected).hexdigest()))
receipt = dict(result='PASS', fixture_cases=4, byte_identical_comparisons=8,
               variants=['normal automatic scratch', 'probe heap scratch', 'probe ASan+UBSan'], files=receipts)
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt))
PY
