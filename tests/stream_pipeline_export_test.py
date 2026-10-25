#!/usr/bin/env python3
"""Validate the JSON from real streaming export in the host runtime fixture."""
import importlib.util, json, sys
from pathlib import Path
files=list(Path(sys.argv[1]).glob('xcloud4-stream-*-p*-11.json'))
assert len(files)==1, files
d=json.loads(files[0].read_text())
assert d['schema']=='X4STREAM_PIPELINE5' and d['profile'] in (0,1)
if len(sys.argv)>2: assert d['profile']==(1 if int(sys.argv[2])==2 else 0)
assert d['ready_policy']=='latest' and d['display_policy']=='one_in_flight'
assert d['native_pipeline_depth']==1 and d['converters']==2 and d['output_memory_type']==0
requested={0:dict(width=1280,height=720,fps=60,bitrate_kbps=10000,os_name='android',display_info=False),
           1:dict(width=1920,height=1080,fps=60,bitrate_kbps=12000,os_name='windows',display_info=True)}
assert d['requested']==requested[d['profile']]
s=d['status'];assert s['native_outputs']==100 and s['accepted_AU']==s['consumed_AU']==100
assert s['error']==0 and s['retained'] is False and s['decoder_close']==0
assert s['width']==1280 and s['height']==720 and s['presentations']>0
assert d['scanout']['peak_in_flight']==1 and d['ring']['final_occupied']==0
waits=d['blocking_waits'];assert waits['worker_timeout_us']==20000
assert {t['name'] for t in waits['threads']}=={'decode','converter0','converter1','main'}
for t in waits['threads']:
    assert t['errors']==t['first_error']==0
    assert t['wakeups']<=t['wait_calls'] and t['timeouts']<=t['wakeups']
    assert t['wakeups_per_second']>=0
assert d['cpu']['variant']==0
assert d['cpu']['native_decoder']['priority']==700 and d['cpu']['native_decoder']['affinity']==0x3f
assert all(not t['priority_attempted'] and not t['affinity_attempted'] and not t['result'] for t in d['cpu']['converters'])
assert not d['cpu']['decode_caller']['result']
assert d['shutdown_waits']['halt_end_us']>=d['shutdown_waits']['halt_begin_us']>0
for key in ('allocate','map','register','rate','close','unmap','release'):assert d['scanout'][key]==0
assert d['delivered_sps']['parsed_during_native_start'] is True
assert d['sps_diagnostics']['syntax_complete'] is True
assert d['dequeued_AU']==100
assert s['display_detached'] is True and s['rejected_width']==s['rejected_height']==0
recovery=d['recovery']
assert recovery['bad_frame_error_limit'] is None and recovery['other_native_error_limit']==10 and recovery['frame_errors']==recovery['consecutive_errors']==0
assert recovery['idr_wait']['retry_us']==300000 and recovery['idr_wait']['timeout_us']==5000000
assert recovery['maximum_consecutive_errors']==0 and recovery['restarts']==recovery['reconfigurations']==0
assert recovery['keyframe_created']==recovery['keyframe_consumed']==recovery['keyframe_coalesced']==0
assert recovery['keyframe_pending'] is False
assert recovery['error_capacity']==32 and recovery['error_records_omitted']==0 and recovery['errors']==[]
assert recovery['epoch_capacity']==8 and recovery['epoch_count']==1 and recovery['epoch_records_omitted']==0
assert len(recovery['epochs'])==1
assert recovery['epochs'][0]['ordinal']==1 and recovery['epochs'][0]['start_result']==0
assert recovery['epochs'][0]['retained'] is False and recovery['epochs'][0]['creation_uncertain'] is False
assert d['latest_observed_sps']['profile_idc']==d['delivered_sps']['profile_idc']
assert d['latest_sps_diagnostics']['syntax_complete'] is True
assert d['latest_parameter_sets']['sps']['hex']=='0000000167'
assert d['parameter_sets']['sps']['hex']=='0000000167'
assert d['parameter_sets']['pps']['hex']=='000000016880'
assert d['sdp_profiles']['accepted_answer'][0]['profile_level_id']=='42e020'
assert d['stacks'][0] is None  # separate assembly owner is not part of this mock
for t in d['stacks'][1:]:assert t['configured_bytes']==262144 and t['create_result']==t['join_result']==0
m=d['metrics'];assert m['presentations']==s['presentations']
if m['presentations']>1:
    assert m['presentation_interval_us']==m['last_present_us']-m['first_present_us']
    assert abs(m['images_per_second']-(m['presentations']-1)*1e6/m['presentation_interval_us'])<1e-6
assert m['stages']['decode']['count']==100
assert m['stages']['decode_end_to_flip_observation']['count']==s['presentations']
assert m['stages']['conversion_end_to_submit']['count']==s['presentations']
assert not any(word in files[0].read_text().lower() for word in ('gstoken','authorization','bearer','password','v=0\\r\\n'))
print('PASS real streaming exporter: schema, actual output geometry, bounded counters, native/mock shutdown and explicit rate denominator')
failed=list(Path(sys.argv[1]).glob('xcloud4-stream-*-p*-38.json'))
assert len(failed)==1
failure=json.loads(failed[0].read_text())
assert failure['status']['error']==-7411 and failure['status']['native_outputs']==0
assert failure['dequeued_AU']==1 and failure['status']['consumed_AU']==1
assert failure['sps_diagnostics']['valid_mask']==(2|131072)
assert failure['sps_diagnostics']['syntax_complete'] is False
assert failure['delivered_sps']['profile_idc']==100 and failure['delivered_sps']['chroma_format_idc']==2
assert failure['latest_observed_sps']['profile_idc']==100 and failure['latest_observed_sps']['chroma_format_idc']==2
assert failure['latest_sps_diagnostics']['syntax_complete'] is False
assert failure['recovery']['epoch_count']==1 and failure['recovery']['epochs'][0]['start_result']==-7411
assert failure['recovery']['frame_errors']==0
for kind in ('sps','pps'):
    assert failure['parameter_sets'][kind]==dict(bytes=80,captured_bytes=64,truncated=True,hex='ab'*64)
print('PASS early rejection export: partial SPS retained, bounded SPS/PPS hex and AU stages remain distinct')
reader_path=Path(__file__).resolve().parents[1]/'scripts/analyze_stream_pipeline.py'
reader_spec=importlib.util.spec_from_file_location('admission_export_reader',reader_path)
reader=importlib.util.module_from_spec(reader_spec);reader_spec.loader.exec_module(reader)
for session,error,label,fatal in ((40,-7604,'AU identity capacity',False),(41,-7440,'AU admission state',True)):
    reports=list(Path(sys.argv[1]).glob(f'xcloud4-stream-*-p*-{session}.json'))
    assert len(reports)==1, reports
    admission=json.loads(reports[0].read_text())
    status=admission['status'];queue=admission['au_queue']
    assert status['error']==error and status['retained'] is False
    assert status['accepted_AU']==status['consumed_AU']==admission['dequeued_AU']==0
    assert status['native_outputs']==status['presentations']==0 and status['decoder_close']==0
    assert admission['recovery']['failure_stage']==label
    assert queue['fatal'] is fatal and queue['stopped'] is True
    assert queue['admitted']==queue['taken']==queue['released']==queue['samples']==0
    assert all(queue[k] is None for k in ('depth_p50','depth_p95','lag_p50_upper_us','lag_p95_upper_us'))
    result=reader.analyze(admission)
    assert result['error']==error and result['retained'] is False
print('PASS real fatal-admission exports and actual reader: deferred labels, zero admissions, empty percentiles and explicit queue fatal')
