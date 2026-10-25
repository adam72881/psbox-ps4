#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Call actual live_h264.c against deterministic HOST mocks, not a console."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
p.add_argument('--sanitize', choices=['none', 'address,undefined'], default='none')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
receipt = {'kind': 'HOST_MOCK_NOT_PS4', 'native_executed': False, 'variants': [], 'compiler': a.cc,
           'sanitize': a.sanitize,
           'coverage': ['ordinary path', 'OFF legacy policy and 1080 refusal',
                        'ON 128 MiB inclusive boundary and rounded overshoot rejection',
                        '106807296 byte planned total admission with synthetic query components',
                        'default output type 3 and experimental type 0',
                        'invalid type refusal before native queries',
                        'cacheable allocation/Create/Decode error and cleanup']}
for probe, capacity in ((0, 0), (1, 0), (1, 1)):
    d = out / ('probe%d-capacity%d' % (probe, capacity)); d.mkdir(exist_ok=True)
    cmd = [a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-pthread', '-ffunction-sections', '-fdata-sections',
           '-DX4_VIDEO_PROBE=%d' % probe, '-DX4_VIDEO_CAPACITY_1080=%d' % capacity,
           '-I' + str(root / 'tests/video_capacity_stubs'), str(root / 'tests/probe_native_mock_test.c'),
           str(root / 'src/video/video_capacity.c'), '-Wl,--gc-sections', '-o', str(d / 'mock')]
    if probe:
        cmd += ['-DX4_PROBE_STACK_HOST=1', str(root / 'src/video/probe_contract.c'), str(root / 'src/video/probe_stack.c')]
    if a.sanitize != 'none':
        cmd += ['-fsanitize=' + a.sanitize, '-fno-omit-frame-pointer']
    build = subprocess.run(cmd, text=True, capture_output=True)
    (d / 'build.stdout.txt').write_text(build.stdout, encoding='utf-8')
    (d / 'build.stderr.txt').write_text(build.stderr, encoding='utf-8')
    r = {'probe': probe, 'capacity': capacity, 'command': cmd, 'build_returncode': build.returncode}
    if build.returncode == 0:
        run = subprocess.run([str(d / 'mock')], text=True, capture_output=True)
        (d / 'run.stdout.txt').write_text(run.stdout, encoding='utf-8')
        (d / 'run.stderr.txt').write_text(run.stderr, encoding='utf-8')
        r.update(run_returncode=run.returncode,
                 binary_sha256=hashlib.sha256((d / 'mock').read_bytes()).hexdigest())
    receipt['variants'].append(r)
receipt['result'] = 'PASS' if all(v.get('run_returncode', -1) == 0 for v in receipt['variants']) else 'FAIL'
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
print(json.dumps(receipt))
raise SystemExit(0 if receipt['result'] == 'PASS' else 1)
