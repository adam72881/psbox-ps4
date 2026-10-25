#!/usr/bin/env python3
"""Synthetic contract tests for the real streaming reader; no PS4 measurements."""
from copy import deepcopy
import importlib.util
from pathlib import Path

root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('real_depth_reader',root/'scripts/analyze_stream_pipeline.py')
reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)

def rejected(fn,value):
    try:fn(value)
    except ValueError:return
    raise AssertionError('invalid report contract accepted')

def attempt(depth,query=0,create=0):
    return dict(depth=depth,kind=0,fallback=False,skipped=False,level_known=True,
        profile=100,level=42,dpb=4,dpb_requested=4,dpb_minimum=4,dpb_level_cap=4,
        reason=0,failure_reason=7 if query or create else 0,
        failure_stage='QUERY DECODER' if query else 'CREATE DECODER' if create else '',
        compute_query=0,allocate_queue=0,query=query,create=create,cleanup=0)

def history():
    return dict(attempts=[attempt(2,-12,reader.NOT_RUN),attempt(1)],attempt_count=2,
        active_attempt=1,requested_depth=2,active_depth=1,depth_fallback_used=True,fallback_used=True)

def data():
    return dict(decoder_depth=dict(selection=0,requested=2,active=2,fallback=False,
        input_buffers=4,output_buffers=5,calls_with_output=2,calls_without_output=1,
        calls_with_native_error=0,output_epochs_with_picture=1,first_output_us=1000,last_output_us=3000,
        native_budget_bytes=192*1024*1024,auxiliary_budget_bytes=32*1024*1024,
        held_output_mask_after_close=0,live_inputs_after_close=0,
        input_retirement_posterior_calls=3,input_contract_SDK_verified=False,
        flush_on_close=False,output_interval_scope='same epoch'),
        native_pipeline_depth=2,status=dict(native_outputs=2,retained=False),
        native=dict(create=0,planned_bytes=127107072),metrics=dict(stages=dict(
            decode=dict(count=3,sum_us=22),decode_with_output=dict(count=2,sum_us=20),
            decode_without_output=dict(count=1,sum_us=2),native_output_interval=dict(count=1,sum_us=2000))))

def slot_epoch_controls():
    def epoch(ordinal,begin,close,depth):
        return dict(ordinal=ordinal,begin_us=begin,close_us=close,decoder_start=dict(active_depth=depth))
    mixed=dict(decoder_depth=dict(active=1),decoder_start=dict(active_depth=1),
        recovery=dict(epoch_records_omitted=0,epochs=[epoch(1,100,200,1),epoch(2,210,300,2),epoch(3,310,0,1)]))
    def completed(at,slot):return dict(observed_us=at,slot=slot,completed_output=True)
    for at in (150,250,350):
        assert reader.validate_error_output_slot(mixed,5,completed(at,3))==3
    assert reader.validate_error_output_slot(mixed,5,completed(250,4))==4
    for at in (150,350):
        rejected(lambda r:reader.validate_error_output_slot(mixed,5,r),completed(at,4))
    rejected(lambda r:reader.validate_error_output_slot(mixed,5,r),completed(250,5))
    # The final depth must never reclassify an earlier SPS epoch.
    final_two=deepcopy(mixed);final_two['decoder_start']['active_depth']=2;final_two['decoder_depth']['active']=2
    rejected(lambda r:reader.validate_error_output_slot(final_two,5,r),completed(150,4))
    for version in (3,4):
        rejected(lambda r:reader.validate_error_output_slot(mixed,version,r),completed(250,4))
    legacy5=deepcopy(mixed);del legacy5['decoder_depth']
    rejected(lambda r:reader.validate_error_output_slot(legacy5,5,r),completed(250,4))
    omitted=deepcopy(mixed);omitted['recovery'].update(epoch_records_omitted=4,epochs=[epoch(5,500,0,1)])
    assert reader.validate_error_output_slot(omitted,5,completed(250,4))==4
    rejected(lambda r:reader.validate_error_output_slot(omitted,5,r),completed(550,4))
    rejected(lambda r:reader.validate_error_output_slot(omitted,5,r),completed(250,5))
    never_started=deepcopy(mixed);never_started['recovery']['epochs']=[epoch(1,100,0,0)]
    rejected(lambda r:reader.validate_error_output_slot(never_started,5,r),completed(150,0))
    # A noncompleted SPS/control record can truthfully carry the new sentinel.
    assert reader.validate_error_output_slot(mixed,5,dict(observed_us=150,slot=5,completed_output=False))==5
    print('PASS real reader slot ownership: legacy3/4 fixed4 despite optional depth; mixed1/2/1 uses observed epoch; omitted epoch conservative5; final depth never rewrites history')


def main():
    slot_epoch_controls()
    reader.validate_attempts(history(),5)
    for change in ('unsafe_cleanup','no_rejection','wrong_depth','backwards','missing_flag'):
        h=history()
        if change=='unsafe_cleanup':h['attempts'][0]['cleanup']=-8
        elif change=='no_rejection':h['attempts'][0].update(query=0,create=0)
        elif change=='wrong_depth':h['attempts'][1]['depth']=2
        elif change=='backwards':h['attempts'][0]['kind']=1;h['attempts'][0]['fallback']=True
        else:h['depth_fallback_used']=False
        rejected(lambda x:reader.validate_attempts(x,5),h)
    d=data();r=reader.validate_decoder_depth(d)
    assert r['input_output_call_delay'] is None
    assert r['native_outputs_per_second_first_to_last']==500
    mutations=(('input_buffers',2),('output_buffers',4),('calls_without_output',0),
        ('output_epochs_with_picture',0),('held_output_mask_after_close',1),
        ('live_inputs_after_close',1),('input_contract_SDK_verified',True),
        ('flush_on_close',True),('input_retirement_posterior_calls',1),
        ('native_budget_bytes',128*1024*1024),('auxiliary_budget_bytes',24*1024*1024),
        ('last_output_us',999))
    for key,value in mutations:
        bad=deepcopy(d);bad['decoder_depth'][key]=value
        rejected(reader.validate_decoder_depth,bad)
    bad=deepcopy(d);bad['metrics']['stages']['decode_without_output']['sum_us']=3
    rejected(reader.validate_decoder_depth,bad)
    # A successful Delete is required before the ledger can claim zero ownership;
    # a retained decoder's report may truthfully preserve its reservations.
    retained=deepcopy(d);retained['status']['retained']=True
    retained['decoder_depth']['held_output_mask_after_close']=3
    retained['decoder_depth']['live_inputs_after_close']=3
    reader.validate_decoder_depth(retained)
    assert reader.validate_decoder_depth({}) is None  # historical reports
    print('PASS real streaming reader: bounded depth fallback, 18 negative controls, delayed-output counts, interval coverage, retained ownership, no PTS inference; synthetic data only')

if __name__=='__main__':main()
