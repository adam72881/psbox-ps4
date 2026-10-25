#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline controlled-clock probe-statistics tests, no native SDK or console."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
parser.add_argument('--sanitize', choices=['none', 'address,undefined', 'thread'], default='none')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
binary = out / ('probe_metrics_test.exe' if os.name == 'nt' else 'probe_metrics_test')
cmd = [args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
       '-DX4_PROBE_METRICS_TEST', str(root / 'tests/probe_metrics_test.c'),
       str(root / 'src/video/probe_metrics.c'), '-o', str(binary)]
if args.sanitize != 'none':
    cmd.extend(['-fsanitize=' + args.sanitize, '-fno-omit-frame-pointer'])
version = subprocess.run([args.cc, '--version'], check=True, text=True, capture_output=True).stdout
build = subprocess.run(cmd, text=True, capture_output=True)
(out / 'build.stdout.txt').write_text(build.stdout, encoding='utf-8')
(out / 'build.stderr.txt').write_text(build.stderr, encoding='utf-8')
receipt = {'commands': [cmd], 'compiler_version': version, 'build_returncode': build.returncode,
           'sanitizers': args.sanitize, 'native_execution': False}
json_checks = 0
def verify(condition):
    global json_checks
    json_checks += 1
    assert condition
if build.returncode == 0:
    run = subprocess.run([str(binary), str(out / 'metrics-fixture.json')], text=True, capture_output=True)
    (out / 'run.stdout.txt').write_text(run.stdout, encoding='utf-8')
    (out / 'run.stderr.txt').write_text(run.stderr, encoding='utf-8')
    receipt.update(run_returncode=run.returncode)
    if run.returncode == 0:
        test_result = json.loads(run.stdout)
        fixture = json.loads((out / 'metrics-fixture.json').read_text(encoding='utf-8'))
        verify(fixture['schema'] == 'X4PROBE_METRICS1')
        verify(fixture['exact_percentile_scope'] == 'last_retained_stable_observations')
        verify(len(fixture['stages']) == 23)
        verify(all(name in fixture['stages'] for name in (
            'copy_owner_loop', 'copy_helper0_loop', 'copy_helper1_loop',
            'copy_helper2_loop', 'copy_readers_max_loop', 'copy_completion_wait',
            'free_output_wait', 'stripe0', 'stripe1', 'stripe2',
            'decode_end_to_flip_observation', 'submit_return_to_flip_observation',
            'conversion_end_to_flip_submit')))
        verify(fixture['omitted_events'] == 1 and fixture['dirty_resets'] == 1)
        verify(fixture['counters']['cancel']['stable_observed'] == 1)
        for stage in fixture['stages'].values():
            verify(len(stage['all_observed_bins']) == 32)
            verify(sum(stage['all_observed_bins']) == stage['accepted_observations'])
            verify(stage['retained'] <= 4096)
            verify(stage['retained'] + stage['overwritten'] == stage['accepted_observations'])
            raw = sorted(stage['retained_samples_us'])
            verify(len(raw) == stage['retained'])
            for percentile in (50, 95, 99):
                observed = raw[(len(raw) * percentile + 99) // 100 - 1] if raw else 0
                verify(observed == stage['retained_p%d_us' % percentile])
            verify(stage['retained_max_us'] == (raw[-1] if raw else 0))
            verify(stage['last_observation_us'] >= stage['first_observation_us'])
            verify(bool(stage['accepted_observations']) == bool(stage['last_observation_us']))
        for counter in fixture['counters'].values():
            verify(counter['timestamped_count'] <= counter['stable_observed'])
            verify(counter['untimed_stable_units'] == counter['stable_observed'] - counter['timestamped_count'])
            verify(counter['measured_last_us'] >= counter['measured_first_us'])
            verify(bool(counter['timestamped_count']) == bool(counter['measured_last_us']))
        timed_fixture = json.loads((out / 'metrics-fixture.json.timestamps').read_text(encoding='utf-8'))
        input_count = timed_fixture['counters']['input_AU']
        verify(input_count['total_observed'] == 67)
        verify(input_count['stable_observed'] == 6)
        verify(input_count['timestamped_count'] == 4)
        verify(input_count['timestamp_omissions'] == 1)
        verify(input_count['untimed_stable_units'] == 2)
        verify(input_count['measured_first_us'] == 1500)
        verify(input_count['measured_last_us'] == 2500)
        untimed_eof = timed_fixture['counters']['EOF']
        verify(untimed_eof['stable_observed'] == 1)
        verify(untimed_eof['timestamped_count'] == 0)
        verify(untimed_eof['measured_first_us'] == untimed_eof['measured_last_us'] == 0)
        aggregate = timed_fixture['counters']['backpressure']
        verify(aggregate['timestamped_count'] == 3)
        verify(aggregate['measured_first_us'] == aggregate['measured_last_us'] == 4500)
        verify(timed_fixture['stages']['copy_including_synchronization']['first_observation_us'] == 2100)
        verify(timed_fixture['stages']['copy_including_synchronization']['last_observation_us'] == 2600)
        conversion_to_submit = timed_fixture['stages']['conversion_end_to_flip_submit']
        verify(conversion_to_submit['accepted_observations'] == 2)
        verify(conversion_to_submit['first_observation_us'] == 3000)
        verify(conversion_to_submit['last_observation_us'] == 3100)
        verify(conversion_to_submit['retained_samples_us'] == [5, 9])
        verify(conversion_to_submit['retained_p50_us'] == 5)
        verify(conversion_to_submit['retained_p95_us'] == 9)
        verify(conversion_to_submit['retained_p99_us'] == 9)
        verify(conversion_to_submit['retained_max_us'] == 9)
        stacks = json.loads((out / 'metrics-fixture.json.stacks').read_text(encoding='utf-8'))['thread_stacks']
        verify(stacks['schema'] == 'X4PROBE_STACK1' and len(stacks['slots']) == 4)
        verify(stacks['missing_slot_is_unknown'] is True)
        for item in stacks['slots'][:3]:
            verify(item['configured_bytes'] == item['reported_bytes'] == 262144)
            verify(item['high_water_bytes'] == 36864)
            verify(item['low_excluded_bytes'] == 4096 and item['top_excluded_bytes'] == 12288)
            verify(item['started'] and item['finished'] and item['pattern_ready'])
            verify('pre-paint' in item['high_water_scope'] and 'undercount' in item['high_water_scope'])
        verify(stacks['slots'][3]['create_rc'] == -17)
        verify(stacks['slots'][3]['name'] == 'x4-"copy')
        verify(stacks['slots'][3]['pattern_ready'] is False)
        verify(fixture['thread_stacks']['slots'] == [None] * 4)
        receipt.update(test_result=test_result, json_reader_checks=json_checks,
                       fixture_sha256=hashlib.sha256((out / 'metrics-fixture.json').read_bytes()).hexdigest(),
                       timestamp_fixture_sha256=hashlib.sha256((out / 'metrics-fixture.json.timestamps').read_bytes()).hexdigest(),
                       binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
print(json.dumps(receipt))
raise SystemExit(receipt.get('run_returncode', build.returncode))
