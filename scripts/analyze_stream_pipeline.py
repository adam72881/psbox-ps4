#!/usr/bin/env python3
"""Read bounded streaming reports; never infer requested media was delivered."""
import argparse
import json
import math
import re
from pathlib import Path

NOT_RUN=-(1<<31)
CPU_NOT_RUN=NOT_RUN+1


def integer(value, minimum=0, maximum=None):
    if type(value) is not int or value<minimum or (maximum is not None and value>maximum):
        raise ValueError('invalid bounded integer')
    return value


def boolean(value):
    if type(value) is not bool:raise ValueError('invalid boolean observation')
    return value


def bounded_text(value,maximum=1024):
    if not isinstance(value,str) or len(value.encode('utf-8'))>maximum or any(ord(c)<32 for c in value):
        raise ValueError('invalid bounded observation text')
    return value


def cpu_return(value):
    return integer(value,minimum=-(1<<31),maximum=(1<<31)-1)


def validate_cpu_snapshot(snapshot):
    attempted=boolean(snapshot['attempted']);valid=boolean(snapshot['valid'])
    cpu_return(snapshot['priority']);integer(snapshot['affinity'],maximum=(1<<64)-1)
    returns=[cpu_return(snapshot[k]) for k in ('priority_result','attr_init_result',
        'attr_get_result','affinity_result','attr_destroy_result')]
    # Raw getter output is only a usable observation when every getter and
    # attribute cleanup succeeded. Invalid snapshots never supply restoration
    # targets or an applied-value comparison to the reader.
    if valid:
        if not attempted or any(returns):raise ValueError('invalid successful CPU snapshot')
        integer(snapshot['priority'],maximum=(1<<31)-1)
        integer(snapshot['affinity'],minimum=1,maximum=(1<<64)-1)
    elif attempted and not any(returns):
        raise ValueError('failed CPU snapshot lacks getter error')
    return valid


def validate_cpu(data):
    cpu=data['cpu'];policy=cpu.get('policy_version')
    if policy is not None and integer(policy)!=2:raise ValueError('unsupported CPU policy version')
    modern=policy==2;variant=integer(cpu['variant'],maximum=3 if modern else 2)
    native=cpu['native_decoder']
    if type(native['start_attempted']) is not bool:raise ValueError('invalid native CPU attempt flag')
    cpu_return(native['priority']);integer(native['affinity'],maximum=(1<<64)-1)
    priority=(700,480,480,256)[variant] if modern else (700,256,256)[variant]
    affinity=15 if variant>=2 else 63
    if native['start_attempted'] and (native['priority'],native['affinity'])!=(priority,affinity):
        raise ValueError('native CPU request differs from selected experiment')
    records=[cpu['decode_caller'],*cpu['converters'],cpu['assembly']]
    if len(cpu['converters'])!=2:raise ValueError('invalid CPU converter coverage')
    for record,identity in zip(records,((0,0),(1,0),(1,1),(2,0))):
        if record is None:continue
        integer(record['variant'],maximum=3 if modern else 2)
        role=integer(record['role'],maximum=2);index=integer(record['index'],maximum=1)
        priority_attempted=boolean(record['priority_attempted'])
        affinity_attempted=boolean(record['affinity_attempted'])
        setter_attempted=priority_attempted or affinity_attempted
        active_variant=record['variant']>=2 if modern else record['variant']==2
        expected_priority=0 if role==0 else 500 if role==2 else 700
        expected_affinity=15 if role==0 else 32 if role==2 else 16<<index
        if setter_attempted:
            if not active_variant:raise ValueError('unexpected CPU application to unchanged role')
            if role==0 and priority_attempted:raise ValueError('Decode caller priority must be inherited')
            if (record['requested_priority'],record['requested_affinity'])!=(expected_priority,expected_affinity):
                raise ValueError('CPU thread experiment differs from captured values')
        if not modern:continue
        result=cpu_return(record['result']);partial=boolean(record['partial_applied'])
        if record['nonfatal'] is not True:raise ValueError('CPU scheduling failure must remain nonfatal')
        outcome=record['outcome']
        if outcome not in ('unchanged','applied','skipped_capture','restored','partial'):
            raise ValueError('invalid CPU application outcome')
        before=record['before'];after=record['after']
        before_valid=validate_cpu_snapshot(before);after_valid=validate_cpu_snapshot(after)
        restore_priority=boolean(record['restore_priority_attempted'])
        restore_affinity=boolean(record['restore_affinity_attempted'])
        for key in ('priority_result','affinity_result','restore_priority_result','restore_affinity_result'):
            cpu_return(record[key])
        observed=setter_attempted or before['attempted'] or after['attempted'] or restore_priority or restore_affinity or result or outcome!='unchanged'
        # Before thread entry, the exporter can contain a wholly zeroed report.
        # It is not an observation of CPU-0 or of the slot's actual thread.
        if observed and (record['variant']!=variant or (role,index)!=identity):
            raise ValueError('CPU thread observation identity mismatch')
        if observed and active_variant and (record['requested_priority'],record['requested_affinity'])!=(expected_priority,expected_affinity):
            raise ValueError('CPU capture does not describe requested experiment')
        if restore_priority and (not priority_attempted or not before_valid):
            raise ValueError('CPU priority restoration lacks valid captured target')
        if restore_affinity and (not affinity_attempted or not before_valid):
            raise ValueError('CPU affinity restoration lacks valid captured target')
        if outcome=='unchanged':
            if observed or partial:raise ValueError('unchanged CPU record claims an operation')
        elif outcome=='skipped_capture':
            if before_valid or not before['attempted'] or after['attempted'] or setter_attempted or restore_priority or restore_affinity or not result or not partial:
                raise ValueError('invalid skipped CPU capture')
        elif outcome=='applied':
            if not before_valid or not after_valid or result or partial or restore_priority or restore_affinity:
                raise ValueError('invalid applied CPU outcome')
            if not affinity_attempted or record['affinity_result'] or (role!=0 and (not priority_attempted or record['priority_result'])):
                raise ValueError('applied CPU outcome lacks successful setters')
            if after['affinity']!=expected_affinity or (role!=0 and after['priority']!=expected_priority):
                raise ValueError('observed CPU application differs from requested values')
        else:
            if not before_valid or not result or not partial:raise ValueError('invalid incomplete CPU application')
            if outcome=='restored' and (not after_valid or (after['priority'],after['affinity'])!=(before['priority'],before['affinity'])):
                raise ValueError('CPU restored outcome differs from captured values')
    if modern:
        partial=any(r is not None and (r['partial_applied'] or r['result']) for r in records)
        if boolean(cpu['partially_applied'])!=partial:raise ValueError('CPU partial application aggregate mismatch')
        if cpu['status_es']!=('parcialmente aplicada' if partial else 'sin fallos de ajuste observados'):
            raise ValueError('CPU application status label mismatch')
        if 'starvation_observation' in cpu:
            observation=cpu['starvation_observation']
            if integer(observation['variant'],maximum=3)!=variant:raise ValueError('CPU starvation observation variant mismatch')
            rejected=integer(observation['rtp_callbacks_rejected'])
            full=integer(observation['rtp_callbacks_full_design_failures'],maximum=rejected)
            if 'rtp_callback_admission' in data:
                final=data['rtp_callback_admission']
                if rejected>final['rejected'] or full>final['full_design_failures']:
                    raise ValueError('CPU callback snapshot exceeds final counters')
            audio=observation['audio'];available=boolean(audio['available'])
            bounded_text(audio['scope']);integer(audio['observed_us'])
            cpu_return(audio['startup_error']);cpu_return(audio['stop_result'])
            for key in ('packets','frames','dropped','lost','bad_frames','underflows','error'):
                if available:
                    cpu_return(audio[key]) if key=='error' else integer(audio[key])
                elif audio[key] is not None:raise ValueError('unavailable audio counter presented as observed')
    return cpu


def validate_diagnostics(sps,diag):
    if type(diag['syntax_complete']) is not bool or sps['parsed_during_native_start']!=diag['syntax_complete']:
        raise ValueError('SPS completeness identity mismatch')
    integer(diag['valid_mask'],maximum=(1<<64)-1)


def validate_parameters(parameters):
    for name in ('sps','pps'):
        parameter=parameters[name]
        size=integer(parameter['bytes'],maximum=4096)
        captured=parameter['captured_bytes'];value=parameter['hex']
        if captured!=min(size,64) or type(parameter['truncated']) is not bool or parameter['truncated']!=(size>captured):
            raise ValueError('invalid bounded parameter coverage')
        if not isinstance(value,str) or len(value)!=captured*2 or not re.fullmatch('[0-9a-f]*',value):
            raise ValueError('invalid parameter hex')


def validate_attempts(start,version):
    attempts=start['attempts'];depth_history=version>=5 and 'requested_depth' in start
    limit=6 if depth_history else 3 if version>=3 else 2
    if depth_history:
        requested_depth=integer(start['requested_depth'],maximum=2)
        integer(start['active_depth'],maximum=2);boolean(start['depth_fallback_used'])
        prior_kind=-1;prior_depth=0;depth_fallbacks=0
    if integer(start['attempt_count'],maximum=limit)!=len(attempts):
        raise ValueError('invalid bounded native retry history')
    if version>=3:
        active=integer(start['active_attempt'],maximum=limit-1)
        if (attempts and active!=len(attempts)-1) or type(start['fallback_used']) is not bool:
            raise ValueError('invalid active native attempt')
        expected_fallback=any(not a['skipped'] and (a['kind']!=0 or a.get('depth',requested_depth)!=requested_depth) for a in attempts) if depth_history else any(i and not a['skipped'] for i,a in enumerate(attempts))
        if start['fallback_used']!=expected_fallback:
            raise ValueError('invalid fallback execution identity')
    for i,a in enumerate(attempts):
        if type(a['fallback']) is not bool or a['fallback']!=((a['kind']!=0) if depth_history else (i!=0)):
            raise ValueError('invalid native retry ordering')
        if version<3:continue
        kind=integer(a['kind'],maximum=2)
        if (not depth_history and kind!=i) or type(a['skipped']) is not bool or type(a['level_known']) is not bool:
            raise ValueError('invalid native policy identity')
        if depth_history:
            depth=integer(a['depth'],minimum=1,maximum=2)
            if prior_kind>kind:raise ValueError('decoder policy moved backwards')
            if kind==prior_kind:
                previous=attempts[i-1]
                if (prior_depth,depth)!=(2,1) or previous['cleanup']!=0 or not any(previous[k] not in (0,NOT_RUN) for k in ('query','create')):
                    raise ValueError('depth retry lacks rejected Query/Create and safe cleanup')
                depth_fallbacks+=1
            elif i and depth!=prior_depth:
                raise ValueError('depth changed outside same-policy fallback')
            elif not i and depth!=requested_depth:
                raise ValueError('first depth attempt differs from request')
            prior_kind,prior_depth=kind,depth
        minimum=integer(a['dpb_minimum'],minimum=4)
        requested=integer(a['dpb_requested'],minimum=minimum)
        cap=integer(a['dpb_level_cap'],maximum=16)
        reason=integer(a['reason'],maximum=8);failure=integer(a['failure_reason'],maximum=8)
        label=a['failure_stage']
        if not isinstance(label,str) or len(label.encode('utf-8'))>47 or any(ord(c)<32 for c in label):
            raise ValueError('invalid bounded native failure stage')
        if (failure or a['skipped']) and not label:
            raise ValueError('native failure reason omitted')
        if failure not in (0,7,8) or (a['skipped'] and reason not in (4,5,6)) or (not a['skipped'] and reason not in (0,1,2,3)):
            raise ValueError('invalid native policy reason')
        dpb=integer(a['dpb'],maximum=16)
        if a['skipped']:
            if any(a[k]!=NOT_RUN for k in ('compute_query','allocate_queue','query','create','cleanup')):
                raise ValueError('skipped attempt performed native operation')
        elif not minimum<=dpb<=min(requested,cap):
            raise ValueError('effective DPB violates minimum or level capacity')
        if (kind==2 if depth_history else i==2) and (a['profile'],a['level'],requested)!=(66,31,4):
            # A rejected legacy ref conflict deliberately retains its actual
            # minimum/request; it must never be presented as a native config.
            if not a['skipped']:raise ValueError('invalid legacy720 configuration')

    if depth_history and bool(depth_fallbacks)!=start['depth_fallback_used']:
        raise ValueError('depth fallback flag differs from attempt history')


def validate_retention(records,count,capacity,omitted):
    integer(count);integer(omitted)
    if len(records)!=min(count,capacity) or omitted!=max(0,count-capacity):
        raise ValueError('bounded record coverage mismatch')
    if [e['ordinal'] for e in records]!=list(range(max(1,count-capacity+1),count+1)):
        raise ValueError('bounded record ordinals mismatch')


def validate_error_output_slot(data,version,record):
    """Validate exact completed ownership against the epoch at error time.

    Historical schemas keep their four-slot contract even if an optional new
    block is injected. A schema5 error preceding the oldest retained epoch
    may belong to an omitted depth2 epoch: use the conservative five-slot
    storage bound there, never infer its depth from the final decoder.
    Noncompleted control/error records may use the schema5 sentinel slot5.
    """
    extended=version>=5 and 'decoder_depth' in data
    slot=integer(record['slot'],maximum=5 if extended else 4)
    if not record['completed_output']:return slot
    capacity=4
    if extended:
        observed=integer(record['observed_us']);recovery=data['recovery']
        epochs=recovery['epochs'];candidates=[]
        for epoch in epochs:
            begin=integer(epoch['begin_us']);closed=integer(epoch['close_us'])
            if begin<=observed and (not closed or observed<=closed):
                candidates.append(epoch)
        if candidates:
            epoch=max(candidates,key=lambda e:(e['begin_us'],e['ordinal']))
            depth=epoch['decoder_start'].get('active_depth')
            if depth is not None:
                depth=integer(depth,maximum=2)
                capacity=5 if depth==2 else 4 if depth==1 else 0
        elif recovery['epoch_records_omitted'] and (not epochs or observed<min(e['begin_us'] for e in epochs)):
            capacity=5
    if slot>=capacity:
        raise ValueError('completed output slot exceeds decoder epoch capacity')
    return slot


def validate_recovery(data,version=3):
    recovery=data['recovery']
    if recovery['error_capacity']!=32 or recovery['epoch_capacity']!=8:
        raise ValueError('unexpected bounded recovery policy')
    if version==3:
        if recovery['frame_error_limit']!=10:raise ValueError('unexpected old recovery policy')
    else:
        if recovery['bad_frame_error_limit'] is not None or recovery['other_native_error_limit']!=10:
            raise ValueError('unexpected IDR recovery policy')
        wait=recovery['idr_wait']
        if wait['retry_us']!=300000 or wait['timeout_us']!=5000000 or type(wait['waiting']) is not bool:
            raise ValueError('invalid bounded IDR wait')
        for key in ('begin_us','absolute_deadline_us','waits','recoveries','timeouts'):integer(wait[key])
        if wait['waits'] and wait['absolute_deadline_us']-wait['begin_us']!=5000000:
            raise ValueError('IDR deadline was renewed or malformed')
        if wait['recoveries']>wait['waits'] or wait['timeouts']>1:raise ValueError('invalid IDR outcomes')
    errors=integer(recovery['frame_errors'])
    maximum=integer(recovery['maximum_consecutive_errors'],maximum=10 if version==3 else None)
    consecutive=integer(recovery['consecutive_errors'],maximum=maximum)
    if version>=4:integer(recovery['consecutive_other_native_errors'],maximum=min(10,consecutive))
    if maximum>errors:raise ValueError('invalid consecutive error coverage')
    created=integer(recovery['keyframe_created']);taken=integer(recovery['keyframe_consumed'])
    if type(recovery['keyframe_pending']) is not bool or taken>created or created-taken!=int(recovery['keyframe_pending']):
        raise ValueError('invalid local keyframe mailbox accounting')
    integer(recovery['keyframe_coalesced'])
    for key in ('restarts','reconfigurations','waiting_idr_drops','discarded_ready_for_reconfiguration',
                'quiesce_calls','quiesce_wait_us','quiesce_wait_max_us','parameter_logs_omitted'):
        integer(recovery[key])
    if recovery['restarts']>errors or recovery['quiesce_wait_max_us']>recovery['quiesce_wait_us']:
        raise ValueError('invalid recovery counters')
    epochs=recovery['epochs'];epoch_count=integer(recovery['epoch_count'])
    validate_retention(epochs,epoch_count,8,recovery['epoch_records_omitted'])
    for epoch in epochs:
        integer(epoch['input_AU']);begin=integer(epoch['begin_us']);closed=integer(epoch['close_us'])
        if closed and closed<begin:raise ValueError('epoch closes before beginning')
        integer(epoch['reason'],maximum=2)
        for key in ('retained','creation_uncertain'):
            if type(epoch[key]) is not bool:raise ValueError('invalid native epoch ownership flag')
        if epoch['creation_uncertain'] and not epoch['retained']:
            raise ValueError('uncertain Create resources not retained')
        validate_diagnostics(epoch['sps'],epoch['sps_diagnostics'])
        validate_parameters(epoch['parameter_sets']);validate_attempts(epoch['decoder_start'],version)
    records=recovery['errors']
    validate_retention(records,errors,32,recovery['error_records_omitted'])
    for record in records:
        kind=integer(record['frame_class'],minimum=1,maximum=4)
        slot=validate_error_output_slot(data,version,record)
        integer(record['input_AU']);integer(record['observed_us'])
        integer(record['consecutive'],minimum=1,maximum=10 if version==3 else None)
        integer(record['native_result'],minimum=-(1<<31),maximum=(1<<31)-1)
        integer(record['control_result'],minimum=-(1<<31),maximum=(1<<31)-1)
        for key in ('accepted','completed_output','ownership_uncertain'):
            if type(record[key]) is not bool:raise ValueError('invalid error ownership flag')
        if record['completed_output'] and record['ownership_uncertain']:
            raise ValueError('ambiguous output claimed completed')
        if kind==1 and (not record['completed_output'] or record['ownership_uncertain'] or record['native_result'] or record['control_result']):
            raise ValueError('damaged output lacks exact completed ownership')
        if kind==2 and (not record['ownership_uncertain'] or record['completed_output']):
            raise ValueError('restart does not preserve uncertain ownership')
        if kind==3 and (record['native_result'] or record['control_result']>=0 or record['accepted'] or record['completed_output'] or record['ownership_uncertain']):
            raise ValueError('SPS control misrepresented as native Decode')
    validate_diagnostics(data['latest_observed_sps'],data['latest_sps_diagnostics'])
    validate_parameters(data['latest_parameter_sets'])
    status=data['status']
    if type(status['display_detached']) is not bool or type(status['retained']) is not bool:
        raise ValueError('invalid final ownership flags')
    integer(status['rejected_width']);integer(status['rejected_height'])
    if data['native']['retained'] and not status['retained']:
        raise ValueError('native resource retention hidden by final status')
    return recovery


def validate_new_diagnostics(metrics):
    started=integer(metrics['started_us']);ended=integer(metrics['ended_us'],minimum=started)
    duration=ended-started;complete_expected=min(duration//1000000,3600)
    cadence=metrics['cadence'];integer(cadence['bucket_origin_us'])
    if cadence['bucket_origin_us']!=started:raise ValueError('cadence origin mismatch')
    for name in ('received_complete_AU','presented'):
        c=cadence[name];complete=integer(c['complete_seconds'],maximum=3600)
        counts=c['counts_by_complete_second']
        if len(counts)!=complete or complete!=complete_expected:raise ValueError('cadence second coverage mismatch')
        for n in counts:integer(n,maximum=65535)
        if c['near_30_bounds']!=[27,33] or c['near_60_bounds']!=[55,65]:raise ValueError('invalid cadence ranges')
        expected=(sum(n==0 for n in counts),sum(27<=n<=33 for n in counts),sum(55<=n<=65 for n in counts))
        if (c['zero_seconds'],c['near_30_seconds'],c['near_60_seconds'])!=expected:
            raise ValueError('cadence histogram mismatch')
        if c['other_seconds']!=complete-sum(expected):raise ValueError('invalid other cadence seconds')
        integer(c['total']);integer(c['omitted_events']);integer(c['seconds_beyond_capacity'])
        integer(c['trailing_partial_duration_us'],maximum=999999)
        if c['trailing_partial_duration_us']!=duration%1000000 or c['seconds_beyond_capacity']!=max(0,duration//1000000-3600):
            raise ValueError('cadence partial/capacity denominator mismatch')
        trailing=c['trailing_partial_count']
        if trailing is not None:integer(trailing,maximum=65535)
        if (trailing is None)!=(duration//1000000>=3600):raise ValueError('cadence trailing bucket coverage mismatch')
        if sum(counts)+(trailing or 0)+c['omitted_events']!=c['total']:
            raise ValueError('cadence event accounting mismatch')
    if cadence['presented']['total']!=metrics['presentations']:raise ValueError('cadence presentation mismatch')
    latency=metrics['keyframe_to_idr']
    for key in ('requests','n','maximum_us','overflow_count','observed_complete_idrs','requests_pending','requests_omitted','requests_ambiguous_publication'):integer(latency[key])
    if latency['n']+latency['requests_pending']+latency['requests_omitted']+latency['requests_ambiguous_publication']!=latency['requests']:
        raise ValueError('keyframe-to-IDR coverage mismatch')
    if latency['requests_pending']>128 or latency['overflow_count']>latency['n'] or latency['histogram_quantum_us']!=1000:
        raise ValueError('unbounded keyframe-to-IDR recording')
    if latency['p50_upper_us'] is not None:integer(latency['p50_upper_us'],maximum=6000000)
    finite_median=latency['n']>0 and latency['n']-latency['overflow_count']>=(latency['n']+1)//2
    if (latency['p50_upper_us'] is not None)!=finite_median:raise ValueError('keyframe-IDR percentile coverage mismatch')
    if finite_median and not 1000<=latency['p50_upper_us']<=latency['maximum_us']+1000:
        raise ValueError('keyframe-IDR percentile exceeds maximum')
    if not latency['n'] and (latency['maximum_us'] or latency['overflow_count']):raise ValueError('empty keyframe-IDR statistics')
    if latency['n'] and not latency['observed_complete_idrs']:raise ValueError('matched keyframe lacks IDR observation')
    if latency['oldest_pending_age_us'] is not None:integer(latency['oldest_pending_age_us'])
    if (latency['oldest_pending_age_us'] is None)!=(latency['requests_pending']==0):raise ValueError('pending request age coverage mismatch')
    pause=metrics['pause_evidence'];names=('unknown','rtp_gap','waiting_idr','decoder','presentation','mixed')
    count=integer(pause['record_count']);omitted=integer(pause['omitted_records'])
    if pause['capacity']!=256 or count!=metrics['intervals_over_100ms']:
        raise ValueError('pause evidence interval mismatch')
    for name in names:integer(pause['counts'][name])
    if sum(pause['counts'].values())!=count:raise ValueError('pause classification count mismatch')
    validate_retention(pause['records'],count,256,omitted)
    def classification(mask):return 'unknown' if not mask else 'mixed' if mask&(mask-1) else names[{1:1,2:2,4:3,8:4}[mask]]
    for r in pause['records']:
        mask=integer(r['evidence_mask'],maximum=15)
        begin=integer(r['begin_us']);end=integer(r['end_us'],minimum=begin+100001)
        if begin<metrics['first_present_us'] or end>metrics['last_present_us'] or r['duration_us']!=end-begin or r['classification']!=classification(mask):
            raise ValueError('pause evidence/label mismatch')
    for name in names:
        retained=sum(r['classification']==name for r in pause['records'])
        if retained>pause['counts'][name] or (not omitted and retained!=pause['counts'][name]):
            raise ValueError('pause retained class counts mismatch')
    terminal=pause['terminal_open_interval'];duration=integer(terminal['duration_us'])
    if type(terminal['over_100ms']) is not bool or terminal['over_100ms']!=(duration>100000):raise ValueError('invalid open interval')
    mask=integer(terminal['evidence_mask'],maximum=15)
    if terminal['classification']!=classification(mask):raise ValueError('invalid open interval evidence')


def validate_color_policy(data):
    """Bind color to the last retained successful start, not native output size.

    A tolerated scalar native output may have different visible dimensions.
    Latest SPS can also belong to a failed/cancelled transition. Successful
    epochs retain the exact SPS used by the policy before the first Decode.
    """
    color=data['color'];preference=data['request_profile']['color_preference']
    matrix=color['received_matrix_coefficients'];effective=color['matrix_coefficients']
    if effective not in (0,1,6):raise ValueError('invalid effective color matrix enum')
    resolved=color['active'] or data['conversion_path']['observed']
    if not resolved:
        return dict(status='not_observed',reason='no active policy or conversion observation; Create alone is insufficient')
    if effective not in (1,6):raise ValueError('resolved color matrix missing')
    # Explicit SPS matrices and the forced compatibility setting have no
    # dependency on resolution. Keep this check even if history was pruned.
    if matrix!=2 or preference==1:
        expected=1 if matrix==1 else 6
        if effective!=expected:raise ValueError('explicit color policy mismatch')
    if color['selected_matrix']!=('BT709' if effective==1 else 'BT601'):
        raise ValueError('effective color label mismatch')
    recovery=data['recovery'];successful=[e for e in recovery['epochs'] if e['start_result']==0]
    if not successful:
        if recovery['epoch_records_omitted']:
            return dict(status='ambiguous',reason='last successful SPS may be outside bounded epoch history',
                        epoch_records_omitted=recovery['epoch_records_omitted'],effective_matrix=effective)
        raise ValueError('resolved color lacks a successful native epoch')
    epoch=successful[-1];sps=epoch['sps']
    if not epoch['sps_diagnostics']['syntax_complete']:
        raise ValueError('successful color epoch lacks complete SPS')
    width=integer(sps['visible_width'],minimum=1,maximum=1920)
    height=integer(sps['visible_height'],minimum=1,maximum=1088)
    source_matrix=integer(sps['matrix_coefficients'],maximum=255)
    if source_matrix!=matrix:raise ValueError('received color differs from last successful SPS')
    expected=1 if matrix==1 or (matrix==2 and preference==0 and height>=720) else 6
    if effective!=expected:raise ValueError('runtime color policy mismatch')
    return dict(status='verified',source='last retained successful native epoch',epoch_ordinal=epoch['ordinal'],
                sps_visible_width=width,sps_visible_height=height,received_matrix=matrix,effective_matrix=effective,
                native_output_visible_width=data['status']['width'],native_output_visible_height=data['status']['height'])


def validate_admission(data):
    q=data['au_queue']; limiter=data['keyframe_limiter']; callbacks=data['rtp_callback_admission']
    for k in ('admitted','taken','released','dropped_full','dropped_late','dropped_wait_idr','dropped_busy',
              'rejected_incoming_full','rejected_incoming_late','rejected_incoming_wait_idr','rejected_incoming_busy',
              'retired_ready_full','retired_ready_late','retired_ready_busy',
              'evicted_non_idr','old_idr_retired','recovery_transitions','request_intentions','request_coalesced',
              'padding_bytes_total','samples','lag_max_us','lag_histogram_overflow','resident_bytes','payload_bytes'):
        integer(q[k])
    if (q['capacity_entries'],q['maximum_AU_bytes'],q['late_limit_us'],q['arena_bytes'])!=(32,2097152,250000,8388608):
        raise ValueError('invalid bounded AU byte ring configuration')
    if (data['AU_queue_bytes'],data['parameter_queue_bytes'])!=(8388608,262144):
        raise ValueError('invalid AU memory accounting')
    metadata=integer(q['metadata_bytes_including_parameters'],minimum=262144)
    if metadata!=data['AU_queue_metadata_bytes_including_parameters']:
        raise ValueError('AU metadata accounting mismatch')
    for k in ('depth_max','resident_entries','ready_entries','inflight_entries'):
        integer(q[k],maximum=32)
    if not data['status']['retained'] and q['inflight_entries']:
        raise ValueError('in-flight AU hidden by clean close')
    if q['inflight_entries']>1 or q['ready_entries']+q['inflight_entries']>q['resident_entries']:
        raise ValueError('invalid AU queue ownership accounting')
    if q['payload_bytes']>q['resident_bytes'] or q['resident_bytes']>q['arena_bytes']:
        raise ValueError('AU queue byte capacity exceeded')
    if not q['samples']:
        if q['depth_p50'] is not None or q['depth_p95'] is not None:raise ValueError('empty queue observations claimed percentiles')
    else:
        integer(q['depth_p50'],maximum=32);integer(q['depth_p95'],maximum=32)
        if q['depth_p50']>q['depth_p95'] or q['depth_p95']>q['depth_max']:
            raise ValueError('invalid queue depth percentiles')
    if q['lag_histogram_quantum_us']!=1000 or q['lag_histogram_overflow']>q['samples']:
        raise ValueError('invalid queue lag coverage')
    for percent in (50,95):
        value=q[f'lag_p{percent}_upper_us']
        finite=q['samples']>0 and q['samples']-q['lag_histogram_overflow'] >= (q['samples']*percent+99)//100
        if (value is not None)!=finite:raise ValueError('queue lag percentile coverage mismatch')
        if value is not None:
            integer(value,maximum=1000000)
            if value>q['lag_max_us']+1000:raise ValueError('queue lag exceeds observed maximum')
    if sum(q['retired_ready_'+k] for k in ('full','late','busy'))!=q['evicted_non_idr']+q['old_idr_retired']:
        raise ValueError('READY retirement reason accounting mismatch')
    if q['admitted']!=q['taken']+q['ready_entries']+q['evicted_non_idr']+q['old_idr_retired']:
        raise ValueError('final AU admission coverage mismatch')
    if q['released']>q['taken'] or q['taken']>q['admitted']:
        raise ValueError('invalid AU lease accounting')
    if (q['admitted'],q['taken'],q['released'])!=(data['status']['accepted_AU'],data['dequeued_AU'],data['status']['consumed_AU']):
        raise ValueError('AU pipeline/queue frontier mismatch')
    for k in ('waiting_idr','stopped','fatal'):
        if type(q[k]) is not bool:raise ValueError('invalid AU queue state')
    if not q['stopped']:raise ValueError('export before AU owners stopped')
    for k in ('requested','coalesced','deferred','granted','retired_on_idr','granted_gap_generation','interval_deferred','cap_deferred','clock_deferred',
              'gap_generation','idr_generation','last_granted_us','transport_decisions','control_attempts',
              'control_local_successes','control_errors','pli_attempts','pli_local_successes','pli_errors'):
        integer(limiter[k])
    integer(limiter['pending_reasons'],maximum=63)
    if (limiter['minimum_interval_us'],limiter['window_us'],limiter['window_limit'])!=(500000,5000000,10):
        raise ValueError('invalid common keyframe rate policy')
    for k in ('attached','counter_overflow'):
        if type(limiter[k]) is not bool:raise ValueError('invalid limiter state')
    if not limiter['counter_overflow']:
        if limiter['deferred']!=limiter['interval_deferred']+limiter['cap_deferred']+limiter['clock_deferred'] or limiter['granted_gap_generation']>limiter['gap_generation']:
            raise ValueError('invalid limiter deferral/generation accounting')
        if limiter['coalesced']>limiter['requested'] or limiter['granted']>limiter['requested']:
            raise ValueError('invalid keyframe intention accounting')
        if limiter['transport_decisions']>limiter['granted']:
            raise ValueError('transport escaped common limiter')
        for mechanism in ('control','pli'):
            if limiter[mechanism+'_attempts']>limiter['transport_decisions'] or limiter[mechanism+'_local_successes']+limiter[mechanism+'_errors']!=limiter[mechanism+'_attempts']:
                raise ValueError('invalid keyframe subattempt accounting')
    integer(callbacks['rejected']);integer(callbacks['full_design_failures'],maximum=callbacks['rejected'])
    if type(callbacks['design_failure']) is not bool or callbacks['design_failure']!=(callbacks['full_design_failures']>0):
        raise ValueError('RTP callback FULL design failure hidden')
    metrics=data['metrics']; stages=metrics['stages']
    for k in ('count','sum_us'):
        if stages['decode_IDR'][k]+stages['decode_non_IDR'][k]!=stages['decode'][k]:
            raise ValueError('Decode IDR/non-IDR coverage mismatch')
    idr=metrics['received_idr'];counts=idr['counts_by_complete_second']
    expected=min((metrics['ended_us']-metrics['started_us'])//1000000,3600)
    if len(counts)!=expected or idr['complete_seconds']!=expected:
        raise ValueError('IDR cadence coverage mismatch')
    for count in counts:integer(count,maximum=65535)
    trailing=idr['trailing_partial_count']
    if trailing is not None:integer(trailing,maximum=65535)
    if sum(counts)+(trailing or 0)+integer(idr['omitted_events'])!=idr['total'] or idr['total']!=metrics['keyframe_to_idr']['observed_complete_idrs']:
        raise ValueError('IDR receive coverage mismatch')
    rate=metrics['idr_per_minute'];n=sum(counts)
    if rate['count']!=n or rate['complete_seconds']!=expected or rate['complete_minute_counts']!=[sum(counts[i:i+60]) for i in range(0,len(counts)//60*60,60)]:
        raise ValueError('IDR minute aggregation mismatch')
    actual=n*60/expected if expected else 0.
    if not math.isfinite(rate['rate']) or abs(rate['rate']-actual)>1e-5:
        raise ValueError('IDR/minute denominator mismatch')
    return {k:data[k] for k in ('au_queue','keyframe_limiter','rtp_callback_admission')}


def validate_decoder_depth(data):
    d=data.get('decoder_depth')
    if d is None:return None
    integer(d['selection'],maximum=2);integer(d['requested'],maximum=2)
    active=integer(d['active'],maximum=2);boolean(d['fallback'])
    inputs=integer(d['input_buffers'],maximum=4);outputs=integer(d['output_buffers'],maximum=5)
    if active and (inputs,outputs)!=((4,5) if active==2 else (2,4)):
        raise ValueError('depth/native buffer counts disagree')
    stages=data['metrics']['stages']
    with_output=integer(d['calls_with_output']);without=integer(d['calls_without_output'])
    integer(d['calls_with_native_error'],maximum=with_output+without)
    if with_output+without!=stages['decode']['count']:
        raise ValueError('Decode output/no-output count coverage mismatch')
    for key in ('count','sum_us'):
        if stages['decode_with_output'][key]+stages['decode_without_output'][key]!=stages['decode'][key]:
            raise ValueError('split Decode timing coverage mismatch')
    if stages['decode_with_output']['count']!=with_output or stages['decode_without_output']['count']!=without:
        raise ValueError('split Decode timings differ from owner counts')
    epochs=integer(d['output_epochs_with_picture']);count=integer(data['status']['native_outputs'])
    if epochs>count or stages['native_output_interval']['count']!=count-epochs:
        raise ValueError('native output interval epoch coverage mismatch')
    first=integer(d['first_output_us']);last=integer(d['last_output_us'])
    if last<first or bool(count)!=bool(first) or bool(count)!=bool(last):
        raise ValueError('invalid native output time span')
    budget=integer(d['native_budget_bytes']);aux=integer(d['auxiliary_budget_bytes'])
    held=integer(d['held_output_mask_after_close'],maximum=31);live=integer(d['live_inputs_after_close'],maximum=4)
    if not data['status']['retained'] and (held or live):
        raise ValueError('native ledger retained after successful close')
    if d['input_contract_SDK_verified'] is not False or d['flush_on_close'] is not False:
        raise ValueError('unsupported native lifetime/close claim')
    if active and d['input_retirement_posterior_calls']!=(3 if active==2 else 1):
        raise ValueError('input retirement policy differs from depth')
    if budget:
        if budget!=(192 if active==2 else 128)*1024*1024:
            raise ValueError('unexpected per-depth budget')
        if active==2 and aux<32*1024*1024:
            raise ValueError('depth-two budget omits scanout or AU queue')
        if data['native']['create']==0 and data['native']['planned_bytes']+aux>budget:
            raise ValueError('native plan exceeds effective combined budget')
    if data['native_pipeline_depth']!=(active or 1):raise ValueError('depth header differs from latest native attempt')
    span=last-first
    return {**d,'native_outputs_per_second_first_to_last':(count-1)*1000000/span if count>1 and span else None,
        'native_rate_scope':'all native validated outputs; first/last native return; includes gaps; not a stable gameplay window',
        'input_output_call_delay':None,'input_output_call_delay_reason':'no returned output PTS or input ticket'}


def _analyze(data):
    if data.get('schema') not in ('X4STREAM_PIPELINE1','X4STREAM_PIPELINE2','X4STREAM_PIPELINE3','X4STREAM_PIPELINE4','X4STREAM_PIPELINE5'):
        raise ValueError('unsupported streaming report schema')
    version=int(data['schema'][-1]);profile=data.get('profile')
    expected={1:dict(width=1280,height=720,fps=60,bitrate_kbps=10000,os_name='android' if version==3 else 'Orbis'),
              2:dict(width=1920,height=1080,fps=60,bitrate_kbps=12000,os_name='windows')}
    if version>=4:
        profile_label=data['request_profile']['label']
        web_play=profile_label in ('1080p-D','1080p-E','1080p-F')
        alias_profiles=profile_label in ('1080p-G','1080p-H','1080p-HQ','720p-HQ','1080p-G8')
        expected={p:dict(width=1920 if p in (1,2,3,5) else 1280,height=1080 if p in (1,2,3,5) else 720,
            fps=60,bitrate_kbps=12000 if p in (1,2,3,5) else 10000,
            os_name='android' if p==0 else 'windows' if alias_profiles else ('tizen' if p==2 else 'windows') if web_play else 'windows' if p==1 else 'tizen',
            display_info=p!=0 if web_play else p in (1,3,4,5)) for p in range(6 if alias_profiles else 4)}
    if profile not in expected or data.get('requested') != expected[profile]:
        raise ValueError('profile/request identity mismatch')
    if version>=4:
        integer(profile,maximum=5 if alias_profiles else 4)
        identity=data['request_profile']
        integer(identity['id'],maximum=5 if alias_profiles else 4)
        labels=('720p60','1080p-G','1080p-H','1080p-HQ','720p-HQ','1080p-G8') if alias_profiles else ('720p60','1080p-D','1080p-E','1080p-F') if web_play else ('720p60','1080p-A','1080p-B','1080p-C')
        if identity['id']!=profile or identity['label']!=labels[profile]:
            raise ValueError('runtime profile identity mismatch')
        preference=integer(identity['color_preference'],maximum=1)
        color=data['color'];matrix=integer(color['received_matrix_coefficients'],maximum=255)
        integer(color['preference'],maximum=1)
        integer(color['matrix_coefficients'],maximum=255)
        observed=data['conversion_path']['observed']
        if type(color['active']) is not bool or type(observed) is not bool:
            raise ValueError('invalid color/conversion observation flag')
        if color['preference']!=preference:
            raise ValueError('runtime color policy mismatch')
    if data.get('display_policy')!='one_in_flight' or data.get('ready_policy')!='latest':
        raise ValueError('unexpected pipeline policy')
    status=data['status']; metrics=data['metrics']; sps=data['delivered_sps']
    count=metrics['presentations']; span=metrics['presentation_interval_us']
    if not isinstance(count,int) or count<0 or count!=status['presentations']:
        raise ValueError('presentation counter mismatch')
    rate=(count-1)*1e6/span if count>1 and span>0 else 0.
    if not math.isfinite(metrics['images_per_second']) or abs(rate-metrics['images_per_second'])>1e-5:
        raise ValueError('presentation denominator mismatch')
    if span != max(0,metrics['last_present_us']-metrics['first_present_us']):
        raise ValueError('presentation time span mismatch')
    stages={}
    for name,stage in metrics['stages'].items():
        n=stage['count']; overflow=stage['overflow_count']
        if not isinstance(n,int) or n<0 or overflow<0 or overflow>n:
            raise ValueError('invalid stage coverage')
        values=[stage.get(f'p{p}_upper_us') for p in (50,95,99)]
        finite=[v for v in values if v is not None]
        if finite != sorted(finite) or any(not isinstance(v,int) or v<0 for v in finite):
            raise ValueError('invalid percentile bounds')
        stages[name]={'count':n,'p50_p95_p99_upper_us':values,
                      'maximum_us':stage['maximum_us'],'overflow_count':overflow}
        if name in ('flip_end_to_conversion_begin','controller_read_interval') and 'mean_us' not in stage:
            raise ValueError('new timing stage lacks explicit mean coverage')
        if 'mean_us' in stage:
            total=integer(stage['sum_us']);maximum=integer(stage['maximum_us'])
            mean=stage['mean_us']
            if n:
                if type(mean) not in (int,float) or not math.isfinite(mean) or mean<0 or abs(mean-total/n)>1e-5:
                    raise ValueError('stage mean does not match sum/count')
                if maximum>total:raise ValueError('stage maximum exceeds total duration')
            elif mean is not None or total or maximum:
                raise ValueError('empty stage claims a duration mean')
            stages[name].update(sum_us=total,mean_us=mean)
    metadata={}
    if version>=2:
        diag=data['sps_diagnostics']
        validate_diagnostics(sps,diag)
        validate_parameters(data['parameter_sets'])
        for scope in ('offered','accepted_answer'):
            seen=set()
            for codec in data['sdp_profiles'][scope]:
                pt=codec['payload_type'];value=codec['profile_level_id']
                if not isinstance(pt,int) or not 0<=pt<128 or pt in seen or not re.fullmatch('[0-9a-f]{6}',value):
                    raise ValueError('invalid bounded SDP codec metadata')
                seen.add(pt);number=int(value,16)
                if (number>>16,(number>>8)&255,number&255)!=(codec['profile_idc'],codec['constraints'],codec['level_idc']):
                    raise ValueError('SDP numeric profile mismatch')
        start=data['decoder_start'];attempts=start['attempts']
        validate_attempts(start,version)
        if data['dequeued_AU']>status['accepted_AU'] or status['consumed_AU']>data['dequeued_AU']:
            raise ValueError('invalid AU stage counts')
        metadata={k:data[k] for k in ('sps_diagnostics','parameter_sets','sdp_profiles','decoder_start','color','dequeued_AU','geometry_warnings','conversion_path')}
        if version>=3:
            validate_recovery(data,version)
            metadata.update({k:data[k] for k in ('recovery','latest_observed_sps','latest_sps_diagnostics','latest_parameter_sets')})
            metadata['final_ownership']={k:status[k] for k in ('retained','display_detached')}
            metadata['rejected_geometry']={k:status[k] for k in ('rejected_width','rejected_height')}
        if version>=4:
            validate_new_diagnostics(metrics)
            metadata['color_policy_validation']=validate_color_policy(data)
            metadata['cadence']=metrics['cadence'];metadata['keyframe_to_idr']=metrics['keyframe_to_idr']
            metadata['pause_evidence']=metrics['pause_evidence']
            metadata['request_profile']=data['request_profile']
    if version>=5:metadata.update(validate_admission(data))
    # Optional observations keep original schema5 and 0.7.45 CPU reports readable.
    if 'cpu' in data:metadata['cpu']=validate_cpu(data)
    if 'main_polling' in data:
        polling=data['main_polling']
        if (polling['in_flight_timeout_us'],polling['idle_timeout_us'],polling['state_owner'])!=(1000,5000,'main'):
            raise ValueError('invalid adaptive main polling policy')
        bounded_text(polling['scope']);metadata['main_polling']=polling
    if 'flip_timing' in data:
        timing=data['flip_timing']
        for key in ('valid_native_timestamps','invalid_native_timestamps','unmatched_previous_flips'):integer(timing[key])
        for key in ('last_raw_ptime','last_submitted_us','last_observed_us'):
            if key in timing:integer(timing[key],maximum=(1<<64)-1)
        if 'last_submitted_us' in timing and 'last_observed_us' in timing and timing['last_submitted_us']>timing['last_observed_us']:
            raise ValueError('flip observation precedes local submission')
        bounded_text(timing['scope']);metadata['flip_timing']=timing
    if 'sdp_bandwidth' in data:
        for scope in ('offer','answer'):
            bandwidth=data['sdp_bandwidth'][scope]
            for key in ('video_sections','AS_lines','first_AS_kbps','malformed_AS_lines'):integer(bandwidth[key],maximum=0xffffffff)
            if not bandwidth['AS_lines'] and bandwidth['first_AS_kbps']:raise ValueError('SDP bandwidth without AS line')
            if profile==5 and scope=='offer' and bandwidth['video_sections']:
                if bandwidth['AS_lines']!=bandwidth['video_sections'] or bandwidth['first_AS_kbps']!=8000 or bandwidth['malformed_AS_lines']:
                    raise ValueError('G8 offer does not contain requested 8000 kbps AS line per video section')
        metadata['sdp_bandwidth']=data['sdp_bandwidth']
    if 'blocking_waits' in data:metadata['blocking_waits']=data['blocking_waits']
    if 'shutdown_waits' in data:metadata['shutdown_waits']=data['shutdown_waits']
    if version>=5 and 'decoder_depth' in data:metadata['decoder_depth']=validate_decoder_depth(data)
    return {'source_id':data['source_id'],'profile':profile,'requested':expected[profile],**metadata,
        'delivered_sps':sps,'native_output':{k:status[k] for k in ('width','height','native_width','native_height','pitch','native_outputs')},
        'native_returns':data['native'],'error':status['error'],'retained':status['retained'],
        'presentation':{'images_per_second':rate,'count':count,'interval_us':span,
            'scope':metrics['rate_scope'],'intervals_over_100ms':metrics['intervals_over_100ms'],
            'longest_interval_us':metrics['longest_interval_us']},
        'received_video_rtp':metrics['received_video_rtp'],'stages':stages,
        'ring':data['ring'],'scanout':data['scanout'],'stacks':data['stacks'],
        'diagnostic_record_cost':data['diagnostic_record_cost'],
        'interpretation':['SPS/native dimensions are observations; requested dimensions are separate.',
            'Latest observed SPS can be rejected and is not proof of native acceptance.',
            'Color policy uses the SPS of the last retained successful native epoch; scalar native output may differ and latest SPS may belong to a failed transition. Pruned history is explicit ambiguity.',
            'Keyframe creation/consumption is local mailbox activity, never network delivery.',
            'Native Decode errors with uncertain ownership require Delete before reuse; no output PTS.',
            'Partial SPS fields are valid only where sps_diagnostics.valid_mask says they were read.',
            'SDP profiles are numeric codec metadata, not the SPS delivered by the server.',
            'No output PTS: native outputs cannot be assigned to current input AUs.',
            'Percentiles are histogram upper bounds; null can mean no sample or overflow.',
            'Rate excludes time outside first/last observed flip; includes internal loads/transitions.',
            'Diagnostic update cost does not measure total Trace/callback overhead.']}

def analyze(data):
    try:
        return _analyze(data)
    except (KeyError,TypeError,AttributeError,IndexError,OverflowError) as error:
        raise ValueError('malformed streaming report structure: '+str(error)) from error


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args(); result=analyze(json.loads(args.report.read_text(encoding='utf-8')))
    text=json.dumps(result,ensure_ascii=False,indent=2,allow_nan=False)+'\n'
    if args.output: args.output.write_text(text,encoding='utf-8')
    else: print(text,end='')

if __name__=='__main__': main()
