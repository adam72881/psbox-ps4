#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read actual exporter/file-layer output from synthetic quiescent contexts.

The native decoder is not executed. The reported timings/counts/colors/stacks
are fixtures, never observations from PS4. Writers and reader are production.
"""
import copy
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.dont_write_bytecode = True
import analyze_probe_pipeline as reader

CASES = {'control-720p30-cb', '720p60-cb', '1080p60-cb', '1080p60-main',
         '1080p60-high', 'S720-CB', 'S1080-CB', 'S1080-HIGH', '1080p60-high-15M'}


def main():
    directory = pathlib.Path(sys.argv[1])
    paths = sorted(directory.glob('*-pipeline-*.json'))
    assert len(paths) == 54, f'Expected 54 actual pipeline files, got {len(paths)}'
    assert len(list(directory.glob('*-metrics-*.json'))) == 54
    combinations = set()
    for path in paths:
        case = reader.bounded_json(path)
        metrics_path = path.with_name(path.name.replace('-pipeline-', '-metrics-'))
        metrics = reader.bounded_json(metrics_path)
        result = reader.analyze(case, metrics)
        assert result['report_validation'] == 'PASS', (path.name, result['issues'])
        assert case['schema'] == 'X4PROBE_PIPELINE3' and result['ready_policy'] == 'latest'
        assert result['scanout_policy']=='one_frame_in_flight'
        assert result['videoout_occupancy']['peak_in_flight']==(1 if case['converters'] else 0)
        assert result['videoout_occupancy']['peak_occupied']==(2 if case['converters'] else 0)
        assert result['videoout_occupancy']['final_occupied']==0
        assert result['stages_us']['conversion_end_to_flip_submit']['retained_p50_us']==20
        assert case['ring']['discarded_ring_full'] == case['ring']['discarded_ready']
        assert 'discarded_latency' in case['ring']
        assert 'native_output_latency_discarded' in result['counters']
        assert result['ring']['discarded_ring_full'] == (3 if case['converters'] else 0)
        assert result['ring']['discarded_latency'] == (7 if case['converters'] else 0)
        assert result['counters']['native_output_superseded']['total'] == result['ring']['discarded_ring_full']
        assert result['counters']['native_output_latency_discarded']['total'] == result['ring']['discarded_latency']
        assert result['outcome'] == 'reported_local_completion_with_three_exact_references'
        assert not result['streaming_support_proven']
        assert not result['console_execution_independently_confirmed']
        assert result['coverage'] == {'omitted_events': 0, 'dirty_resets': 0, 'events_after_end': 0}
        combination = case['case_id'], case['mode'], case['converters']
        assert combination not in combinations
        combinations.add(combination)
        assert case['source_id'] == case['build_id'][:40]
        # The build identity includes the independently selected stream mode.
        # Probe files keep the already-validated fixed filename components;
        # their format deliberately predates and does not encode stream mode.
        build_suffix = case['build_id'][40:]
        assert build_suffix.endswith('-sp0'), case['build_id']
        suffix = re.escape(build_suffix[:-4])
        exact = (r'xcloud4-probe-' + re.escape(case['source_id'][:12]) + suffix + '-' +
                 re.escape(case['case_id'] + '-' + case['mode']) + r'-p' + str(case['converters']) +
                 r'-pipeline-' + str(case['started_us']) + r'\.json')
        assert re.fullmatch(exact, path.name), path.name
        assert case['sample']['AU_count'] == case['native_outputs'] == case['input_AU']
        assert case['thread_stacks'] == metrics['thread_stacks']
        # No missing/wrong pair may be reclassified as a successful export.
        bad = copy.deepcopy(metrics)
        bad['created_us'] += 1
        assert reader.analyze(case, bad)['report_validation'] != 'PASS'
        assert reader.analyze(case, {})['report_validation'] != 'PASS'
    assert combinations == {(case, mode, workers) for case in CASES
                            for mode in ('paced', 'maximum-speed') for workers in (0, 2, 3)}
    summary = {'state': 'PASS', 'pairs': len(paths), 'sample_ids': len(CASES),
               'writer': 'real export_pipeline + probe_files.c + probe_metrics.c',
               'inputs_are_synthetic_quiescent_states': True,
               'native_decoder_executed': False, 'console_execution': False,
               'missing_or_mismatched_metrics_rejected': 2 * len(paths)}
    (directory / 'host-export-analysis.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
