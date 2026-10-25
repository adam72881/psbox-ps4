#!/usr/bin/env python3
"""Exercise the actual reader with JSON from the real host runtime exporter."""
import copy
import importlib.util
import json
import sys
from pathlib import Path
reader=Path(__file__).resolve().parents[1]/'scripts/analyze_stream_pipeline.py'
spec=importlib.util.spec_from_file_location('stream_reader',reader)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
data=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
new_data=copy.deepcopy(data)
result=module.analyze(data)
assert result['requested']==data['requested']
assert result['delivered_sps']==data['delivered_sps']
assert result['presentation']['count']==data['status']['presentations']
for mutation in ('schema','request','count','rate','span','percentile','hex','coverage','codec','attempts','partial','AU'):
    bad=copy.deepcopy(data)
    if mutation=='schema':bad['schema']='unknown'
    elif mutation=='request':bad['requested']['width']=1280 if data['requested']['width']==1920 else 1920
    elif mutation=='count':bad['status']['presentations']+=1
    elif mutation=='rate':bad['metrics']['images_per_second']+=1
    elif mutation=='span':bad['metrics']['presentation_interval_us']+=1
    elif mutation=='percentile':bad['metrics']['stages']['decode']['p50_upper_us']=-1
    elif mutation=='hex':bad['parameter_sets']['sps']['hex']='G0'
    elif mutation=='coverage':bad['parameter_sets']['pps']['captured_bytes']=65
    elif mutation=='codec':bad['sdp_profiles']['accepted_answer'][0]['level_idc']+=1
    elif mutation=='attempts':bad['decoder_start']['attempt_count']=len(bad['decoder_start']['attempts'])+1
    elif mutation=='partial':bad['sps_diagnostics']['syntax_complete']=not bad['sps_diagnostics']['syntax_complete']
    else:bad['dequeued_AU']=bad['status']['accepted_AU']+1
    try:module.analyze(bad)
    except ValueError:pass
    else:raise AssertionError('reader accepted '+mutation)
# Explicit old-schema reader fixture, not a claim that these requests happened.
if data['schema']=='X4STREAM_PIPELINE5':
    historical=copy.deepcopy(data);historical['schema']='X4STREAM_PIPELINE4'
    assert module.analyze(historical)['presentation']==result['presentation']
    data=historical
    # The historical V4 colour/startup mutation fixtures below must keep
    # exercising V4; they do not fabricate V5 queue admission counters.
    new_data['schema']='X4STREAM_PIPELINE4'
if data['schema']=='X4STREAM_PIPELINE4':
    data=copy.deepcopy(data);data['schema']='X4STREAM_PIPELINE3'
    data['profile']=1 if data['requested']['width']==1280 else 2
    del data['requested']['display_info']
    data['recovery']['frame_error_limit']=10
    result=module.analyze(data)
# Old reports remain readable without inventing missing diagnostic fields.
for version in (1,2):
    old=copy.deepcopy(data);old['schema']='X4STREAM_PIPELINE'+str(version)
    if old['profile']==1:old['requested']['os_name']='Orbis'
    old['decoder_start']['attempts']=old['decoder_start']['attempts'][:2]
    old['decoder_start']['attempt_count']=len(old['decoder_start']['attempts'])
    assert module.analyze(old)['presentation']==result['presentation']
# These are reader-contract fixtures, not native decoder experiments.
current=copy.deepcopy(data);current['schema']='X4STREAM_PIPELINE3'
if current['profile']==1:current['requested']['os_name']='android'
current['decoder_start']['attempts']=[];current['decoder_start']['attempt_count']=0
current['decoder_start']['active_attempt']=0;current['decoder_start']['fallback_used']=False
# Recovery fields are supplied by the real V3 exporter below; V1/V2 do not
# acquire invented epochs or recovery outcomes merely by being readable.
if data['schema']=='X4STREAM_PIPELINE3':
    assert module.analyze(current)['presentation']==result['presentation']
    wrong_os=copy.deepcopy(current);wrong_os['requested']['os_name']='Orbis'
    try:module.analyze(wrong_os)
    except ValueError:pass
    else:raise AssertionError('V3 accepted historical Orbis request')
    for mutation in ('limit','mailbox','error_omissions','epoch_omissions','latest_hex','latest_partial','retained'):
        bad=copy.deepcopy(data)
        if mutation=='limit':bad['recovery']['frame_error_limit']=11
        elif mutation=='mailbox':bad['recovery']['keyframe_consumed']=bad['recovery']['keyframe_created']+1
        elif mutation=='error_omissions':bad['recovery']['error_records_omitted']+=1
        elif mutation=='epoch_omissions':bad['recovery']['epoch_records_omitted']+=1
        elif mutation=='latest_hex':bad['latest_parameter_sets']['sps']['hex']='G0'
        elif mutation=='latest_partial':bad['latest_sps_diagnostics']['syntax_complete']=not bad['latest_sps_diagnostics']['syntax_complete']
        else:bad['native']['retained']=True;bad['status']['retained']=False
        try:module.analyze(bad)
        except ValueError:pass
        else:raise AssertionError('V3 reader accepted '+mutation)
    # Bounded reader history uses the exact production schema, but these
    # synthetic records do not claim that native recovery happened on PS4.
    history=copy.deepcopy(data);recovery=history['recovery']
    recovery.update(frame_errors=45,consecutive_errors=0,maximum_consecutive_errors=2,
                    keyframe_created=10,keyframe_consumed=9,keyframe_pending=True,keyframe_coalesced=35,
                    restarts=15,quiesce_calls=15,quiesce_wait_us=100,quiesce_wait_max_us=10,
                    error_records_omitted=13,errors=[],epoch_count=12,epoch_records_omitted=4,epochs=[])
    for ordinal in range(14,46):
        kind=1+(ordinal%3)
        recovery['errors'].append(dict(ordinal=ordinal,input_AU=ordinal,observed_us=ordinal*100,
            frame_class=kind,slot=0 if kind==1 else 4,consecutive=1,
            native_result=-303 if kind==2 else 0,control_result=-7403 if kind==3 else 0,
            accepted=kind!=3,completed_output=kind==1,ownership_uncertain=kind==2))
    for ordinal in range(5,13):
        epoch=copy.deepcopy(data['recovery']['epochs'][0])
        epoch.update(ordinal=ordinal,begin_us=ordinal*100,close_us=ordinal*100+50)
        recovery['epochs'].append(epoch)
    module.analyze(history)
    for mutation in ('error_ordinal','completed_ambiguous','bad_slot','restart_ownership','control_native','epoch_ordinal','uncertain_create','epoch_hex'):
        bad=copy.deepcopy(history);errors=bad['recovery']['errors']
        if mutation=='error_ordinal':errors[0]['ordinal']-=1
        elif mutation=='completed_ambiguous':next(e for e in errors if e['frame_class']==1)['ownership_uncertain']=True
        elif mutation=='bad_slot':next(e for e in errors if e['frame_class']==1)['slot']=4
        elif mutation=='restart_ownership':next(e for e in errors if e['frame_class']==2)['ownership_uncertain']=False
        elif mutation=='control_native':next(e for e in errors if e['frame_class']==3)['native_result']=-303
        elif mutation=='epoch_ordinal':bad['recovery']['epochs'][0]['ordinal']-=1
        elif mutation=='uncertain_create':bad['recovery']['epochs'][0]['creation_uncertain']=True
        else:bad['recovery']['epochs'][0]['parameter_sets']['sps']['hex']='G0'
        try:module.analyze(bad)
        except ValueError:pass
        else:raise AssertionError('V3 reader accepted history '+mutation)
    # Synthetic three-attempt metadata exercises the reader's policy bounds;
    # real adapter/native-call behavior is tested by stream_native_test.c.
    current['decoder_start'].update(attempt_count=3,active_attempt=2,fallback_used=True)
    for i in range(3):
        current['decoder_start']['attempts'].append(dict(
            kind=i,fallback=i!=0,profile=66 if i==2 else 100,level=(99,32,31)[i],
            coded_width=1280,coded_height=720,dpb=(8,5,4)[i],dpb_requested=(8,8,4)[i],
            dpb_minimum=4,dpb_level_cap=(16,5,5)[i],level_known=i!=0,skipped=False,
            reason=(3,2,0)[i],failure_reason=7 if i<2 else 0,failure_stage='QUERY DECODER' if i<2 else '',
            compute_query=0,allocate_queue=0,query=-1 if i<2 else 0,create=module.NOT_RUN if i<2 else 0,cleanup=0))
    module.analyze(current)
    for mutation in ('kind','minimum','cap','skipped_native','reason','active','stage','fourth'):
        bad=copy.deepcopy(current);attempt=bad['decoder_start']['attempts'][1]
        if mutation=='kind':attempt['kind']=0
        elif mutation=='minimum':attempt['dpb_minimum']=6
        elif mutation=='cap':attempt['dpb_level_cap']=4
        elif mutation=='skipped_native':attempt['skipped']=True;attempt['reason']=4
        elif mutation=='reason':attempt['reason']=8
        elif mutation=='active':bad['decoder_start']['active_attempt']=0
        elif mutation=='stage':attempt['failure_stage']='x'*48
        else:bad['decoder_start']['attempts'].append(copy.deepcopy(attempt));bad['decoder_start']['attempt_count']=4
        try:module.analyze(bad)
        except ValueError:pass
        else:raise AssertionError('V3 reader accepted retry '+mutation)
for failed in Path(sys.argv[1]).parent.glob('xcloud4-stream-*-p*-38.json'):
    failure=module.analyze(json.loads(failed.read_text()))
    assert failure['error']==-7411 and not failure['sps_diagnostics']['syntax_complete']
if new_data['schema']=='X4STREAM_PIPELINE4':
    # Reader contract fixtures, not claims of console/native execution.
    cancelled=copy.deepcopy(new_data)
    cancelled['native']['create']=0
    cancelled['recovery']['epochs'][-1]['start_result']=3
    cancelled['color'].update(active=False,received_matrix_coefficients=0,matrix_coefficients=0,selected_matrix='BT601')
    cancelled['conversion_path']['observed']=False
    cancelled['status'].update(accepted_AU=2,consumed_AU=0,native_outputs=0,converted_outputs=0,presentations=0,width=0,height=0)
    cancelled['dequeued_AU']=1
    cm=cancelled['metrics'];cm.update(presentations=0,first_present_us=0,last_present_us=0,presentation_interval_us=0,images_per_second=0.,intervals_over_100ms=0,longest_interval_us=0)
    cp=cm['cadence']['presented'];cp.update(total=0,omitted_events=0,zero_seconds=cp['complete_seconds'],near_30_seconds=0,near_60_seconds=0,other_seconds=0,counts_by_complete_second=[0]*cp['complete_seconds'])
    if cp['trailing_partial_count'] is not None:cp['trailing_partial_count']=0
    for stage in cm['stages'].values():stage.update(count=0,sum_us=0,maximum_us=0,overflow_count=0,p50_upper_us=None,p95_upper_us=None,p99_upper_us=None,mean_us=None)
    cm['pause_evidence'].update(record_count=0,omitted_records=0,records=[],counts=dict(unknown=0,rtp_gap=0,waiting_idr=0,decoder=0,presentation=0,mixed=0),terminal_open_interval=dict(duration_us=0,over_100ms=False,evidence_mask=0,classification='unknown'))
    cancelled_result=module.analyze(cancelled)
    assert cancelled_result['presentation']['count']==0 and cancelled_result['color_policy_validation']['status']=='not_observed'
    # The latest SPS/attempt may describe a failed transition, while status
    # retains the last converted geometry and its already-resolved policy.
    changed=copy.deepcopy(new_data)
    changed['color'].update(active=False,received_matrix_coefficients=2,matrix_coefficients=1,selected_matrix='BT709',preference=0)
    changed['request_profile']['color_preference']=0
    changed['status'].update(width=1280,height=720)
    changed['delivered_sps'].update(visible_width=640,visible_height=480,coded_width=640,coded_height=480)
    changed['latest_observed_sps'].update(visible_width=640,visible_height=480,coded_width=640,coded_height=480)
    changed['recovery']['epochs'][-1]['sps'].update(matrix_coefficients=2,visible_width=1280,visible_height=720)
    changed_result=module.analyze(changed)
    assert changed_result['color']['matrix_coefficients']==1 and changed_result['color_policy_validation']['status']=='verified'
    # Scalar native output is permitted to differ from SPS. Policy was fixed
    # from the 720-line SPS before Decode; native 480 lines must not imply 601.
    native_different=copy.deepcopy(changed)
    native_different['delivered_sps'].update(visible_width=1280,visible_height=720,coded_width=1280,coded_height=720,matrix_coefficients=2)
    native_different['latest_observed_sps'].update(native_different['delivered_sps'])
    native_different['status'].update(width=640,height=480,native_width=640,native_height=480,pitch=640)
    native_different['conversion_path']['last']='scalar'
    native_result=module.analyze(native_different)
    assert native_result['color_policy_validation']['sps_visible_height']==720
    assert native_result['color_policy_validation']['native_output_visible_height']==480
    assert native_result['color_policy_validation']['effective_matrix']==1
    # Eight retained failed starts can hide an older successful epoch. Do not
    # invent its SPS height or call the current/native geometry its source.
    pruned=copy.deepcopy(native_different);pr=pruned['recovery'];template=copy.deepcopy(pr['epochs'][-1])
    pr.update(epoch_count=10,epoch_records_omitted=2,epochs=[])
    for ordinal in range(3,11):
        epoch=copy.deepcopy(template);epoch.update(ordinal=ordinal,start_result=-10101)
        pr['epochs'].append(epoch)
    assert module.analyze(pruned)['color_policy_validation']['status']=='ambiguous'
    for matrix,effective in ((1,1),(5,6),(6,6)):
        explicit=copy.deepcopy(pruned)
        explicit['color'].update(received_matrix_coefficients=matrix,matrix_coefficients=effective,
            selected_matrix='BT709' if effective==1 else 'BT601')
        assert module.analyze(explicit)['color_policy_validation']['status']=='ambiguous'
        explicit['color'].update(matrix_coefficients=6 if effective==1 else 1,
            selected_matrix='BT601' if effective==1 else 'BT709')
        try:module.analyze(explicit)
        except ValueError:pass
        else:raise AssertionError('reader accepted wrong explicit matrix with pruned history')
    no_success=copy.deepcopy(pruned);nr=no_success['recovery']
    nr.update(epoch_count=8,epoch_records_omitted=0)
    for ordinal,epoch in enumerate(nr['epochs'],1):epoch['ordinal']=ordinal
    try:module.analyze(no_success)
    except ValueError:pass
    else:raise AssertionError('reader accepted resolved color without any successful native epoch')
    reference_identity=module.analyze(new_data)
    for profile in range(6):
        variant=copy.deepcopy(new_data);variant['profile']=profile
        variant['request_profile']['id']=profile
        if profile<4:
            variant['request_profile']['label']=('720p60','1080p-A','1080p-B','1080p-C')[profile]
            variant['requested']=dict(width=1280 if profile==0 else 1920,height=720 if profile==0 else 1080,
                fps=60,bitrate_kbps=10000 if profile==0 else 12000,
                os_name='android' if profile==0 else 'windows' if profile==1 else 'tizen',display_info=profile in (1,3))
            assert module.analyze(variant)['requested']==variant['requested']
            variant['request_profile']['label']=('720p60','1080p-D','1080p-E','1080p-F')[profile]
            variant['requested'].update(os_name='android' if profile==0 else 'tizen' if profile==2 else 'windows',
                display_info=profile!=0)
            assert module.analyze(variant)['requested']==variant['requested']
            if profile:
                bad_web=copy.deepcopy(variant);bad_web['requested']['display_info']=False
                try:module.analyze(bad_web)
                except ValueError:pass
                else:raise AssertionError('reader accepted web play without displayInfo')
        variant['request_profile']['label']=('720p60','1080p-G','1080p-H','1080p-HQ','720p-HQ','1080p-G8')[profile]
        high_resolution=profile in (1,2,3,5)
        variant['requested']=dict(width=1920 if high_resolution else 1280,height=1080 if high_resolution else 720,
            fps=60,bitrate_kbps=12000 if high_resolution else 10000,
            os_name='android' if profile==0 else 'windows',display_info=profile in (1,3,4,5))
        alias_result=module.analyze(variant)
        assert alias_result['requested']==variant['requested']
        # Identity-only addition: every metric/diagnostic result is unchanged.
        for key in ('presentation','stages','cadence','keyframe_to_idr','pause_evidence'):
            assert alias_result[key]==reference_identity[key]
        for mutation in ('display','os'):
            bad_alias=copy.deepcopy(variant)
            if mutation=='display':bad_alias['requested']['display_info']=not variant['requested']['display_info']
            else:bad_alias['requested']['os_name']='tizen'
            try:module.analyze(bad_alias)
            except ValueError:pass
            else:raise AssertionError('reader accepted invalid alias cohort '+mutation)
    # Explicit metadata-only fixtures: not a claim of native CPU application.
    cpu_fixture=copy.deepcopy(new_data)
    def cpu_record(role,index,priority,affinity,attempted):
        return dict(variant=2,role=role,index=index,requested_priority=priority,requested_affinity=affinity,
            priority_attempted=attempted,priority_result=0 if attempted else -2147483647,
            affinity_attempted=attempted,affinity_result=0 if attempted else -2147483647,result=0)
    cpu_fixture['cpu']=dict(variant=2,native_decoder=dict(start_attempted=True,priority=256,affinity=15),
        decode_caller=dict(cpu_record(0,0,0,15,False),affinity_attempted=True,affinity_result=0),converters=[cpu_record(1,0,700,16,True),cpu_record(1,1,700,32,True)],
        assembly=cpu_record(2,0,500,32,True))
    assert module.analyze(cpu_fixture)['cpu']==cpu_fixture['cpu']
    for mutation in ('variant','native','affinity'):
        bad_cpu=copy.deepcopy(cpu_fixture)
        if mutation=='variant':bad_cpu['cpu']['variant']=3
        elif mutation=='native':bad_cpu['cpu']['native_decoder']['priority']=700
        else:bad_cpu['cpu']['converters'][0]['requested_affinity']=32
        try:module.analyze(bad_cpu)
        except ValueError:pass
        else:raise AssertionError('reader accepted wrong CPU experiment '+mutation)
    g8=copy.deepcopy(variant)
    assert g8['profile']==5
    g8['sdp_bandwidth']=dict(offer=dict(video_sections=1,AS_lines=1,first_AS_kbps=8000,malformed_AS_lines=0),
        answer=dict(video_sections=1,AS_lines=0,first_AS_kbps=0,malformed_AS_lines=0))
    assert module.analyze(g8)['sdp_bandwidth']==g8['sdp_bandwidth']
    g8['sdp_bandwidth']['offer']['first_AS_kbps']=12000
    try:module.analyze(g8)
    except ValueError:pass
    else:raise AssertionError('reader accepted G8 with unchanged 12000 AS offer')
    for mutation in ('deadline','retry','bad_limit','cadence','cadence_total','pause','pause_coverage','request_coverage'):
        bad=copy.deepcopy(new_data)
        if mutation=='deadline':bad['recovery']['idr_wait'].update(waits=1,begin_us=100,absolute_deadline_us=6000100)
        elif mutation=='retry':bad['recovery']['idr_wait']['retry_us']=10000000
        elif mutation=='bad_limit':bad['recovery']['bad_frame_error_limit']=10
        elif mutation=='cadence':bad['metrics']['cadence']['received_complete_AU']['near_30_seconds']+=1
        elif mutation=='cadence_total':bad['metrics']['cadence']['presented']['total']+=1
        elif mutation=='pause':bad['metrics']['pause_evidence']['counts']['unknown']+=1
        elif mutation=='pause_coverage':bad['metrics']['pause_evidence']['omitted_records']+=1
        else:bad['metrics']['keyframe_to_idr']['requests_pending']+=1
        try:module.analyze(bad)
        except ValueError:pass
        else:raise AssertionError('V4 reader accepted '+mutation)
v5=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
if v5['schema']=='X4STREAM_PIPELINE5':
    for mutation in ('arena','admitted','inflight','lag','callback_full','transport','decode_split','idr_minute'):
        bad=copy.deepcopy(v5)
        if mutation=='arena':bad['au_queue']['arena_bytes']+=1
        elif mutation=='admitted':bad['au_queue']['admitted']+=1
        elif mutation=='inflight':bad['au_queue']['inflight_entries']=2
        elif mutation=='lag':bad['au_queue']['lag_p50_upper_us']=-1
        elif mutation=='callback_full':bad['rtp_callback_admission'].update(rejected=1,full_design_failures=1,design_failure=False)
        elif mutation=='transport':bad['keyframe_limiter']['transport_decisions']=bad['keyframe_limiter']['granted']+1
        elif mutation=='decode_split':bad['metrics']['stages']['decode_IDR']['count']+=1
        else:bad['metrics']['idr_per_minute']['complete_minute_counts'].append(1)
        try:module.analyze(bad)
        except ValueError:pass
        else:raise AssertionError('V5 reader accepted '+mutation)
def expect_reader_rejection(report,label):
    try:module.analyze(report)
    except ValueError:return
    raise AssertionError('reader accepted '+label)


def cpu_snapshot(valid=False,priority=700,affinity=63):
    return dict(attempted=valid,valid=valid,priority=priority if valid else 0,
        affinity=affinity if valid else 0,**{k:0 if valid else module.CPU_NOT_RUN for k in
        ('priority_result','attr_init_result','attr_get_result','affinity_result','attr_destroy_result')})


def cpu_v2_record(variant,role,index):
    active=variant>=2;priority=0 if role==0 else 500 if role==2 else 700
    affinity=15 if role==0 else 32 if role==2 else 16<<index
    return dict(variant=variant,role=role,index=index,requested_priority=priority if active else 0,
        requested_affinity=affinity if active else 0,priority_attempted=active and role!=0,
        priority_result=0 if active and role!=0 else module.CPU_NOT_RUN,
        affinity_attempted=active,affinity_result=0 if active else module.CPU_NOT_RUN,
        result=0,nonfatal=True,outcome='applied' if active else 'unchanged',partial_applied=False,
        restore_priority_attempted=False,restore_priority_result=module.CPU_NOT_RUN,
        restore_affinity_attempted=False,restore_affinity_result=module.CPU_NOT_RUN,
        before=cpu_snapshot(active),after=cpu_snapshot(active,priority if role!=0 else 700,affinity))


def cpu_v2_fixture(variant):
    report=copy.deepcopy(new_data);report['status']['error']=0
    report['cpu']=dict(policy_version=2,variant=variant,partially_applied=False,
        status_es='sin fallos de ajuste observados',native_decoder=dict(start_attempted=True,
        priority=(700,480,480,256)[variant],affinity=15 if variant>=2 else 63),
        decode_caller=cpu_v2_record(variant,0,0),
        converters=[cpu_v2_record(variant,1,0),cpu_v2_record(variant,1,1)],
        assembly=cpu_v2_record(variant,2,0),starvation_observation=dict(variant=variant,
        rtp_callbacks_rejected=0,rtp_callbacks_full_design_failures=0,audio=dict(
        available=True,observed_us=100,startup_error=0,error=0,stop_result=0,
        packets=100,frames=100,dropped=0,lost=0,bad_frames=0,underflows=0,
        scope='synthetic pre-stop reader fixture; no firmware starvation claim')))
    return report


if new_data['schema']=='X4STREAM_PIPELINE4':
    # Reader fixtures below are explicit metadata controls. Their CPU failures
    # never become a video error or a claim of native firmware scheduling.
    for variant in range(4):
        report=cpu_v2_fixture(variant);parsed=module.analyze(report)
        assert parsed['cpu']==report['cpu'] and parsed['error']==0
        legacy=copy.deepcopy(report);del legacy['cpu']['policy_version']
        if variant<3:
            legacy['cpu']['native_decoder']['priority']=700 if not variant else 256
            assert module.analyze(legacy)['cpu']['native_decoder']['priority']==(700 if not variant else 256)
        else:expect_reader_rejection(legacy,'legacy CPU-3 without policy_version')
    for outcome in ('skipped_capture','restored','partial'):
        report=cpu_v2_fixture(2);cpu=report['cpu'];record=cpu['converters'][0]
        cpu.update(partially_applied=True,status_es='parcialmente aplicada')
        record.update(outcome=outcome,partial_applied=True,result=-17)
        if outcome=='skipped_capture':
            record.update(priority_attempted=False,affinity_attempted=False,
                priority_result=module.CPU_NOT_RUN,affinity_result=module.CPU_NOT_RUN)
            record['before']=cpu_snapshot();record['before'].update(attempted=True,
                priority_result=-17,priority=-999,affinity=0)
            record['after']=cpu_snapshot()
        elif outcome=='restored':
            record.update(priority_result=-17,affinity_attempted=False,affinity_result=module.CPU_NOT_RUN,
                restore_priority_attempted=True,restore_priority_result=0)
            record['after']=copy.deepcopy(record['before'])
        else:
            record.update(affinity_result=-17,restore_priority_attempted=True,restore_priority_result=0,
                restore_affinity_attempted=True,restore_affinity_result=-19)
        parsed=module.analyze(report)
        assert parsed['error']==0 and parsed['cpu']['partially_applied']
    # A failed after-getter must remain unavailable, rather than validate its
    # raw output as though it had observed applied scheduling values.
    getter_failed=cpu_v2_fixture(3);cpu=getter_failed['cpu'];record=cpu['decode_caller']
    cpu.update(partially_applied=True,status_es='parcialmente aplicada')
    record.update(outcome='partial',partial_applied=True,result=-23)
    record['after'].update(valid=False,priority_result=-23,priority=-999,affinity=0)
    assert module.analyze(getter_failed)['error']==0
    # Threads that never entered may leave zeroed records. These are not
    # observations of CPU-0 or of the slot's role/index.
    early=cpu_v2_fixture(3);cpu=early['cpu']
    zero=cpu_v2_record(0,0,0)
    for key in ('priority_result','affinity_result','restore_priority_result','restore_affinity_result'):zero[key]=0
    for snapshot in (zero['before'],zero['after']):
        for key in ('priority_result','attr_init_result','attr_get_result','affinity_result','attr_destroy_result'):snapshot[key]=0
    cpu.update(native_decoder=dict(start_attempted=False,priority=0,affinity=0),
        decode_caller=copy.deepcopy(zero),converters=[copy.deepcopy(zero),copy.deepcopy(zero)],assembly=None)
    assert module.analyze(early)['cpu']['variant']==3
    unavailable=cpu_v2_fixture(2);audio=unavailable['cpu']['starvation_observation']['audio']
    audio['available']=False
    for key in ('packets','frames','dropped','lost','bad_frames','underflows','error'):audio[key]=None
    assert module.analyze(unavailable)['cpu']['starvation_observation']['audio']['packets'] is None
    for mutation in ('policy','variant','native','aggregate','label','nonfatal','identity','valid_getter',
                     'applied_after','restore_without_capture','starvation_variant','audio_unavailable'):
        bad=cpu_v2_fixture(2);cpu=bad['cpu'];record=cpu['converters'][0]
        if mutation=='policy':cpu['policy_version']=3
        elif mutation=='variant':cpu['variant']=4
        elif mutation=='native':cpu['native_decoder']['priority']=256
        elif mutation=='aggregate':cpu['partially_applied']=True
        elif mutation=='label':cpu['status_es']='aplicada'
        elif mutation=='nonfatal':record['nonfatal']=False
        elif mutation=='identity':record['index']=1
        elif mutation=='valid_getter':record['before']['priority_result']=-17
        elif mutation=='applied_after':record['after']['affinity']=63
        elif mutation=='restore_without_capture':record['restore_affinity_attempted']=True;record['before']['valid']=False
        elif mutation=='starvation_variant':cpu['starvation_observation']['variant']=1
        else:cpu['starvation_observation']['audio']['available']=False
        expect_reader_rejection(bad,'CPU policy v2 '+mutation)
    timing=cpu_v2_fixture(0)
    timing['main_polling']=dict(in_flight_timeout_us=1000,idle_timeout_us=5000,state_owner='main',scope='reader fixture')
    timing['flip_timing']=dict(valid_native_timestamps=2,invalid_native_timestamps=1,
        unmatched_previous_flips=1,last_raw_ptime=9999,last_submitted_us=100,last_observed_us=200,
        scope='reader fixture; deliberately invalid raw ptime is retained without becoming a valid duration')
    timing['metrics']['stages']['flip_end_to_conversion_begin']=dict(count=2,sum_us=100,maximum_us=60,
        overflow_count=0,p50_upper_us=50,p95_upper_us=100,p99_upper_us=100,mean_us=50.0)
    timing['metrics']['stages']['controller_read_interval']=dict(count=0,sum_us=0,maximum_us=0,
        overflow_count=0,p50_upper_us=None,p95_upper_us=None,p99_upper_us=None,mean_us=None)
    parsed=module.analyze(timing)
    assert parsed['stages']['flip_end_to_conversion_begin']['mean_us']==50.0
    assert parsed['stages']['controller_read_interval']['mean_us'] is None
    assert parsed['main_polling']==timing['main_polling'] and parsed['flip_timing']==timing['flip_timing']
    for mutation in ('mean','empty_mean','missing_mean','polling','flip_counter','raw_negative','observation_before_submit'):
        bad=copy.deepcopy(timing)
        if mutation=='mean':bad['metrics']['stages']['flip_end_to_conversion_begin']['mean_us']=49.0
        elif mutation=='empty_mean':bad['metrics']['stages']['controller_read_interval']['mean_us']=0
        elif mutation=='missing_mean':del bad['metrics']['stages']['flip_end_to_conversion_begin']['mean_us']
        elif mutation=='polling':bad['main_polling']['in_flight_timeout_us']=5000
        elif mutation=='flip_counter':bad['flip_timing']['invalid_native_timestamps']=-1
        elif mutation=='raw_negative':bad['flip_timing']['last_raw_ptime']=-1
        else:bad['flip_timing']['last_submitted_us']=201
        expect_reader_rejection(bad,'new timing '+mutation)
    print('PASS CPU reader policy v2: four variants, nonfatal getter/setter/restore outcomes, legacy policy, zeroed startup records, starvation coverage and timing means')
print('PASS actual report reader: source request/observation, coverage, rate, malformed identities and eight V5 queue/limiter/IDR negatives')
