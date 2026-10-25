#!/usr/bin/env python3
"""Read the actual scheduler CPU-2 partial-adjustment report, without PS4 claims."""
import importlib.util
import json
import sys
from pathlib import Path

directory=Path(sys.argv[1])
reports=list(directory.glob('xcloud4-stream-*-p*-120.json'))
assert len(reports)==1,reports
report=json.loads(reports[0].read_text(encoding='utf-8'))
status=report['status'];cpu=report['cpu']
assert cpu['policy_version']==2 and cpu['variant']==2
assert cpu['partially_applied'] is True and cpu['status_es']=='parcialmente aplicada'
assert (status['error'],status['retained'],status['native_outputs'])==(0,False,7)
assert status['presentations']>=2 and status['display_detached'] is True
assert cpu['native_decoder']['priority']==480 and cpu['native_decoder']['affinity']==15
starvation=cpu['starvation_observation']
assert (starvation['variant'],starvation['rtp_callbacks_rejected'],starvation['rtp_callbacks_full_design_failures'])==(2,2,1)
audio=starvation['audio'];assert audio['available'] is True
assert tuple(audio[k] for k in ('packets','frames','dropped','lost','bad_frames','underflows','error'))==(100,98,1,2,3,4,-77)
assert audio['stop_result']==0 and audio['observed_us']>0
partial=cpu['converters'][1]
assert partial['nonfatal'] is True and partial['partial_applied'] is True
assert partial['result']==partial['affinity_result']==-13 and partial['outcome']=='restored'
assert (partial['requested_priority'],partial['requested_affinity'])==(700,32)
assert partial['priority_result']==partial['restore_priority_result']==partial['restore_affinity_result']==0
assert partial['restore_priority_attempted'] is True and partial['restore_affinity_attempted'] is True
for observation in ('before','after'):
    snapshot=partial[observation]
    assert snapshot['attempted'] is True and snapshot['valid'] is True
    assert (snapshot['priority'],snapshot['affinity'])==(612,45)
    assert all(snapshot[k]==0 for k in ('priority_result','attr_init_result','attr_get_result','affinity_result','attr_destroy_result'))
reader_path=Path(__file__).resolve().parents[1]/'scripts/analyze_stream_pipeline.py'
spec=importlib.util.spec_from_file_location('cpu_partial_reader',reader_path)
reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
analysis=reader.analyze(report)
assert analysis['error']==0 and analysis['retained'] is False
print('PASS actual CPU-2 partial scheduler export and production reader: converter1 affinity failure, captured/observed restoration, later video output and clean close')
