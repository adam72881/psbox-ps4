/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/stream_au_queue.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *payload;
static uint8_t sps[] = {0x67, 0x64, 0, 0x28};
static uint8_t pps[] = {0x68, 0x12, 0x34};
static unsigned hook_action;
static bool hook_ran;
static X4StreamAuOutcome hook_out;

static X4StreamAuInput input(size_t bytes, uint64_t identity, bool idr)
{
    X4StreamAuInput v = {0};
    v.data=payload;v.sps=sps;v.pps=pps;v.bytes=bytes;
    v.sps_bytes=sizeof(sps);v.pps_bytes=sizeof(pps);
    v.identity=identity;v.is_idr=idr;v.pts_us=identity*16667;
    return v;
}
static X4StreamAuStats snapshot(X4StreamAuQueue *q)
{
    X4StreamAuStats s;
    assert(x4_stream_au_queue_try_snapshot(q,&s)==X4_STREAM_AU_OK);
    assert(s.resident_bytes<=X4_STREAM_AU_ARENA_BYTES);
    assert(s.resident_entries<=X4_STREAM_AU_ENTRIES);
    assert(s.retired_ready_full+s.retired_ready_late+s.retired_ready_busy==s.evicted_non_idr+s.old_idr_retired);
    return s;
}
static void close_queue(X4StreamAuQueue *q)
{
    x4_stream_au_queue_stop(q);
    assert(x4_stream_au_queue_destroy(q));
}
void x4_stream_au_queue_test_hook(X4StreamAuQueue *q,unsigned event)
{
    if(hook_ran || !hook_action)return;
    if(hook_action==1 && event==X4_STREAM_AU_AFTER_RESERVE) {
        hook_ran=true;
        X4StreamAuLease lease;X4StreamAuView view;
        assert(x4_stream_au_queue_try_take(q,10,&lease,&view,NULL)==X4_STREAM_AU_EMPTY);
        X4StreamAuStats s=snapshot(q);
        assert(s.resident_entries==1 && s.ready_entries==0 && s.inflight_entries==0);
    } else if(hook_action==2 && event==X4_STREAM_AU_BEFORE_PUBLISH) {
        hook_ran=true;
        assert(x4_stream_au_queue_require_idr(q,X4_STREAM_AU_LOSS_FULL,&hook_out)==X4_STREAM_AU_OK);
        assert(hook_out.request_keyframe && hook_out.waiting_idr);
        /* Apply recovery while the only reservation is still WRITING. */
        assert(snapshot(q).ready_entries==0);
    } else if(hook_action==3 && event==X4_STREAM_AU_BEFORE_PUBLISH) {
        hook_ran=true;x4_stream_au_queue_stop(q);
        assert(!x4_stream_au_queue_destroy(q));
    } else if(hook_action==4 && event==X4_STREAM_AU_AFTER_TRY_GATE) {
        hook_ran=true;
        X4StreamAuStats unchanged;memset(&unchanged,0xa5,sizeof(unchanged));
        X4StreamAuStats before=unchanged;
        assert(x4_stream_au_queue_try_snapshot(q,&unchanged)==X4_STREAM_AU_BUSY);
        assert(memcmp(&before,&unchanged,sizeof(before))==0);
        X4StreamAuLease lease;X4StreamAuView view;
        assert(x4_stream_au_queue_try_take(q,10,&lease,&view,NULL)==X4_STREAM_AU_BUSY);
        X4StreamAuInput nested=input(64,99,false);
        assert(x4_stream_au_queue_try_push(q,&nested,10,&hook_out)==X4_STREAM_AU_BUSY);
        assert(hook_out.request_keyframe && hook_out.waiting_idr);
    } else if(hook_action==5 && event==X4_STREAM_AU_BEFORE_READY_STORE) {
        hook_ran=true;
        assert(x4_stream_au_queue_require_idr(q,X4_STREAM_AU_LOSS_FULL,&hook_out)==X4_STREAM_AU_OK);
        assert(snapshot(q).ready_entries==0);
    } else if(hook_action==6 && event==X4_STREAM_AU_BEFORE_TAKE_CLAIM) {
        hook_ran=true;
        assert(x4_stream_au_queue_require_idr(q,X4_STREAM_AU_LOSS_FULL,&hook_out)==X4_STREAM_AU_OK);
    }
}
static void test_owned_spans_and_fifo(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    memset(payload,0x51,1024);
    X4StreamAuInput a=input(1001,1,true),b=input(73,2,false);
    assert(x4_stream_au_queue_try_push(q,&a,100,NULL)==X4_STREAM_AU_OK);
    memset(payload,0x92,1024);sps[1]=0x42;pps[1]=0x56;
    assert(x4_stream_au_queue_try_push(q,&b,101,NULL)==X4_STREAM_AU_OK);
    X4StreamAuLease one,two;X4StreamAuView v;
    assert(x4_stream_au_queue_try_take(q,102,&one,&v,NULL)==X4_STREAM_AU_OK);
    assert(v.identity==1 && v.bytes==1001 && v.data[0]==0x51 && v.data[1000]==0x51);
    assert(v.sps[1]==0x64 && v.pps[1]==0x12 && v.is_idr);
    assert(x4_stream_au_queue_try_take(q,102,&two,&v,NULL)==X4_STREAM_AU_BUSY);
    X4StreamAuLease wrong=one;++wrong.generation;
    assert(x4_stream_au_queue_release(q,wrong)==X4_STREAM_AU_FATAL);
    wrong=one;wrong.generation|=UINT64_C(1)<<61;
    assert(x4_stream_au_queue_release(q,wrong)==X4_STREAM_AU_FATAL);
    assert(snapshot(q).inflight_entries==1);
    assert(x4_stream_au_queue_release(q,one)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_release(q,one)==X4_STREAM_AU_FATAL);
    assert(x4_stream_au_queue_try_take(q,103,&two,&v,NULL)==X4_STREAM_AU_OK);
    assert(v.identity==2 && v.bytes==73 && v.data[0]==0x92 && v.sps[1]==0x42 && v.pps[1]==0x56);
    assert(x4_stream_au_queue_release(q,two)==X4_STREAM_AU_OK);
    X4StreamAuStats s=snapshot(q);
    assert(s.admitted==2 && s.taken==2 && s.released==2 && s.resident_entries==0);
    assert(s.arena_bytes==8*1024*1024 && s.metadata_bytes>=32*8192);
    printf("queue allocation: arena=%zu metadata=%zu total=%zu bytes\n",
        s.arena_bytes,s.metadata_bytes,s.arena_bytes+s.metadata_bytes);
    fflush(stdout);
    sps[1]=0x64;pps[1]=0x12;close_queue(q);
}
static void test_entries_include_inflight(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuInput a=input(32,1,true);memset(payload,0x77,32);
    assert(x4_stream_au_queue_try_push(q,&a,1,NULL)==X4_STREAM_AU_OK);
    X4StreamAuLease pinned;X4StreamAuView v;
    assert(x4_stream_au_queue_try_take(q,2,&pinned,&v,NULL)==X4_STREAM_AU_OK);
    const uint8_t *pinned_data=v.data;
    for(unsigned i=1;i<32;++i){a.identity=i+1;a.is_idr=false;
        assert(x4_stream_au_queue_try_push(q,&a,3+i,NULL)==X4_STREAM_AU_OK);}
    X4StreamAuStats s=snapshot(q);assert(s.resident_entries==32 && s.depth_max==32 && s.inflight_entries==1);
    a.identity=33;X4StreamAuOutcome out;
    assert(x4_stream_au_queue_try_push(q,&a,40,&out)==X4_STREAM_AU_DROPPED_FULL);
    assert(out.request_keyframe && out.waiting_idr);
    s=snapshot(q);assert(s.evicted_non_idr==31 && s.inflight_entries==1 && s.resident_entries==32);
    assert(s.retired_ready_full==31 && s.retired_ready_late==0 && s.retired_ready_busy==0);
    a.is_idr=true;
    assert(x4_stream_au_queue_try_push(q,&a,41,&out)==X4_STREAM_AU_DROPPED_FULL);
    assert(!out.request_keyframe && out.waiting_idr && pinned_data[0]==0x77);
    assert(x4_stream_au_queue_release(q,pinned)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_try_push(q,&a,42,&out)==X4_STREAM_AU_OK);
    assert(!out.waiting_idr && !out.request_keyframe);
    assert(x4_stream_au_queue_release(q,pinned)==X4_STREAM_AU_FATAL);
    X4StreamAuLease lease;
    assert(x4_stream_au_queue_try_take(q,43,&lease,&v,NULL)==X4_STREAM_AU_OK);
    assert(v.is_idr && v.identity==33);
    assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);
    s=snapshot(q);assert(s.request_intentions==1 && s.recovery_transitions==1 && s.dropped_full==2 && s.rejected_incoming_full==2);
    close_queue(q);
}
static void test_byte_capacity_and_contiguous_wrap(void)
{
    const size_t mib=1024*1024;
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuInput a=input(2*mib,1,true);
    for(unsigned i=0;i<4;++i){a.identity=i+1;a.is_idr=i==0;
        assert(x4_stream_au_queue_try_push(q,&a,1+i,NULL)==X4_STREAM_AU_OK);}
    X4StreamAuStats s=snapshot(q);assert(s.resident_entries==4 && s.resident_bytes==8*mib);
    a.bytes=1;a.is_idr=false;X4StreamAuOutcome out;
    assert(x4_stream_au_queue_try_push(q,&a,10,&out)==X4_STREAM_AU_DROPPED_FULL);
    assert(out.request_keyframe);s=snapshot(q);
    assert(s.old_idr_retired==1 && s.evicted_non_idr==3 && s.resident_bytes==0);
    assert(s.retired_ready_full==4 && s.rejected_incoming_full==1);
    close_queue(q);

    q=x4_stream_au_queue_create();assert(q);
    for(unsigned i=0;i<4;++i){a=input(i==3?mib:2*mib,i+1,i==0);
        memset(payload,(int)(i+1),a.bytes);
        assert(x4_stream_au_queue_try_push(q,&a,20+i,NULL)==X4_STREAM_AU_OK);}
    X4StreamAuLease lease;X4StreamAuView v;
    assert(x4_stream_au_queue_try_take(q,24,&lease,&v,NULL)==X4_STREAM_AU_OK);
    const uint8_t *arena_start=v.data;
    assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);
    a=input(2*mib,5,false);memset(payload,5,a.bytes);
    assert(x4_stream_au_queue_try_push(q,&a,25,NULL)==X4_STREAM_AU_OK);
    s=snapshot(q);assert(s.resident_bytes==8*mib && s.payload_bytes==7*mib && s.padding_bytes_total==mib);
    for(unsigned i=2;i<=5;++i){assert(x4_stream_au_queue_try_take(q,25+i,&lease,&v,NULL)==X4_STREAM_AU_OK);
        assert(v.identity==i && v.data[0]==i && v.data[v.bytes-1]==i);
        if(i==5)assert(v.data==arena_start && v.bytes==2*mib);
        assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);}
    assert(snapshot(q).resident_bytes==0);close_queue(q);
}
static void test_lag_and_recovery(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuInput a=input(128,1,true);X4StreamAuOutcome out;
    assert(x4_stream_au_queue_try_push(q,&a,100,NULL)==X4_STREAM_AU_OK);
    X4StreamAuLease lease;X4StreamAuView v;
    assert(x4_stream_au_queue_try_take(q,250100,&lease,&v,&out)==X4_STREAM_AU_OK);
    assert(!out.request_keyframe);assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);
    a.identity=2;
    assert(x4_stream_au_queue_try_push(q,&a,300000,NULL)==X4_STREAM_AU_OK);
    a.is_idr=false;a.identity=3;
    assert(x4_stream_au_queue_try_push(q,&a,300001,NULL)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_try_take(q,600000,&lease,&v,&out)==X4_STREAM_AU_DROPPED_LATE);
    assert(out.request_keyframe && out.waiting_idr);
    for(unsigned i=0;i<50;++i)
        assert(x4_stream_au_queue_try_push(q,&a,600001+i,&out)==X4_STREAM_AU_DROPPED_WAIT_IDR && !out.request_keyframe);
    X4StreamAuStats s=snapshot(q);
    assert(s.old_idr_retired==1 && s.evicted_non_idr==1 && s.dropped_late==1);
    assert(s.retired_ready_late==2 && s.rejected_incoming_late==0 && s.rejected_incoming_wait_idr==50);
    assert(s.request_intentions==1 && s.dropped_wait_idr==50 && s.lag_max_us>=300000);
    assert(s.lag_p95_upper_us>=s.lag_p50_upper_us);
    a.is_idr=true;a.identity=4;
    assert(x4_stream_au_queue_try_push(q,&a,600100,&out)==X4_STREAM_AU_OK && !out.waiting_idr);
    a.is_idr=false;a.identity=5;
    assert(x4_stream_au_queue_try_push(q,&a,600101,NULL)==X4_STREAM_AU_OK);
    for(unsigned i=4;i<=5;++i){assert(x4_stream_au_queue_try_take(q,600102,&lease,&v,NULL)==X4_STREAM_AU_OK);
        assert(v.identity==i);assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);}
    a.is_idr=true;a.arrival_us=1;
    assert(x4_stream_au_queue_try_push(q,&a,900000,&out)==X4_STREAM_AU_DROPPED_LATE && out.request_keyframe);
    assert(snapshot(q).rejected_incoming_late==1);
    close_queue(q);
}
static void test_recovery_cause_fixed_per_episode(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuInput a=input(64,1,true);X4StreamAuOutcome first,second;
    assert(x4_stream_au_queue_try_push(q,&a,1,NULL)==X4_STREAM_AU_OK);
    a.is_idr=false;assert(x4_stream_au_queue_try_push(q,&a,2,NULL)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_require_idr(q,X4_STREAM_AU_LOSS_BUSY,&first)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_require_idr(q,X4_STREAM_AU_LOSS_FULL,&second)==X4_STREAM_AU_OK);
    assert(first.request_keyframe && !second.request_keyframe && second.recovery_generation>first.recovery_generation);
    X4StreamAuStats s=snapshot(q);
    assert(s.retired_ready_busy==2 && !s.retired_ready_full && !s.retired_ready_late);
    assert(s.old_idr_retired==1 && s.evicted_non_idr==1 && s.request_intentions==1);
    a.is_idr=true;assert(x4_stream_au_queue_try_push(q,&a,3,&first)==X4_STREAM_AU_OK && !first.waiting_idr);
    assert(x4_stream_au_queue_require_idr(q,X4_STREAM_AU_LOSS_LATE,&second)==X4_STREAM_AU_OK && second.request_keyframe);
    s=snapshot(q);assert(s.retired_ready_busy==2 && s.retired_ready_late==1 && s.request_intentions==2);
    close_queue(q);
}
static void test_rtp_modular_lag(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuInput a=input(64,1,true);a.rtp_timestamp_valid=true;
    a.rtp_timestamp=UINT32_MAX-9000;
    assert(x4_stream_au_queue_try_push(q,&a,1000,NULL)==X4_STREAM_AU_OK);
    a.is_idr=false;a.rtp_timestamp+=27000;X4StreamAuOutcome out;
    assert(x4_stream_au_queue_try_push(q,&a,2000,&out)==X4_STREAM_AU_DROPPED_LATE);
    assert(out.request_keyframe && snapshot(q).lag_max_us==300000);close_queue(q);
    q=x4_stream_au_queue_create();assert(q);
    a=input(64,1,true);a.rtp_timestamp_valid=true;a.rtp_timestamp=90000;
    assert(x4_stream_au_queue_try_push(q,&a,1000,NULL)==X4_STREAM_AU_OK);
    a.is_idr=false;a.rtp_timestamp=1000;
    assert(x4_stream_au_queue_try_push(q,&a,2000,&out)==X4_STREAM_AU_OK);
    assert(!out.waiting_idr && snapshot(q).lag_max_us==1000);close_queue(q);
}
static void test_hooks_and_cancellation(void)
{
    for(unsigned action=1;action<=4;++action){
        X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
        if(action==4){X4StreamAuInput seeded=input(64,100,true);
            assert(x4_stream_au_queue_try_push(q,&seeded,0,NULL)==X4_STREAM_AU_OK);}
        hook_action=action;hook_ran=false;memset(&hook_out,0,sizeof(hook_out));
        X4StreamAuInput a=input(64,1,false);
        X4StreamAuResult rc=x4_stream_au_queue_try_push(q,&a,1,NULL);
        hook_action=0;assert(hook_ran);
        assert(rc==(action==1?X4_STREAM_AU_OK:action==3?X4_STREAM_AU_STOPPED:X4_STREAM_AU_DROPPED_WAIT_IDR));
        X4StreamAuStats s=snapshot(q);
        if(action==1)assert(s.admitted==1 && s.ready_entries==1);
        if(action==2)assert(s.admitted==0 && s.resident_entries==0 && s.waiting_idr);
        if(action==4)assert(s.dropped_busy==1 && s.request_intentions==1 && s.waiting_idr);
        close_queue(q);
    }
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuInput a=input(64,1,true);
    assert(x4_stream_au_queue_try_push(q,&a,1,NULL)==X4_STREAM_AU_OK);
    X4StreamAuLease lease;X4StreamAuView v;
    assert(x4_stream_au_queue_try_take(q,2,&lease,&v,NULL)==X4_STREAM_AU_OK);
    x4_stream_au_queue_stop(q);assert(!x4_stream_au_queue_destroy(q));
    assert(!x4_stream_au_queue_quiescent(q));
    assert(x4_stream_au_queue_try_push(q,&a,3,NULL)==X4_STREAM_AU_STOPPED);
    assert(x4_stream_au_queue_try_take(q,3,&lease,&v,NULL)==X4_STREAM_AU_STOPPED);
    assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_quiescent(q));
    assert(x4_stream_au_queue_destroy(q));

    q=x4_stream_au_queue_create();assert(q);hook_action=5;hook_ran=false;
    a=input(64,1,false);
    assert(x4_stream_au_queue_try_push(q,&a,1,NULL)==X4_STREAM_AU_OK && hook_ran);
    hook_action=0;X4StreamAuStats s=snapshot(q);
    assert(s.resident_entries==0 && s.evicted_non_idr==1 && s.retired_ready_full==1 && s.admitted==1 && s.waiting_idr);
    close_queue(q);
    q=x4_stream_au_queue_create();assert(q);a=input(64,1,false);
    assert(x4_stream_au_queue_try_push(q,&a,1,NULL)==X4_STREAM_AU_OK);
    hook_action=6;hook_ran=false;
    assert(x4_stream_au_queue_try_take(q,2,&lease,&v,NULL)==X4_STREAM_AU_DROPPED_WAIT_IDR && hook_ran);
    hook_action=0;s=snapshot(q);
    assert(s.inflight_entries==0 && s.taken==0 && s.evicted_non_idr==1 && s.retired_ready_full==1 && s.resident_entries==0);
    close_queue(q);
}
static void test_empty_poll_does_not_touch_gate_or_histogram(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuLease lease;X4StreamAuView view;
    for(unsigned i=0;i<10000;++i)
        assert(x4_stream_au_queue_try_take(q,i,&lease,&view,NULL)==X4_STREAM_AU_EMPTY);
    assert(snapshot(q).samples==0);
    X4StreamAuInput a=input(64,1,true);
    assert(x4_stream_au_queue_try_push(q,&a,10001,NULL)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_try_take(q,10002,&lease,&view,NULL)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);
    X4StreamAuStats before=snapshot(q);
    for(unsigned i=0;i<10000;++i)
        assert(x4_stream_au_queue_try_take(q,20000+i,&lease,&view,NULL)==X4_STREAM_AU_EMPTY);
    assert(snapshot(q).samples==before.samples && before.samples==2);
    close_queue(q);
}
static void test_invalid_limits(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    X4StreamAuInput a=input(X4_STREAM_AU_MAX_BYTES+1,1,true);
    assert(x4_stream_au_queue_try_push(q,&a,1,NULL)==X4_STREAM_AU_FATAL);
    assert(snapshot(q).fatal);assert(x4_stream_au_queue_destroy(q));
    q=x4_stream_au_queue_create();assert(q);a=input(64,1,true);a.sps_bytes=4097;
    assert(x4_stream_au_queue_try_push(q,&a,1,NULL)==X4_STREAM_AU_FATAL);
    assert(x4_stream_au_queue_destroy(q));
}
static void test_simulated_decode_20ms_with_90ms_spikes(void)
{
    /* Discrete events: producer arrival is never moved to Decode completion.
     * This verifies queue admission/ownership, not PS4 scheduling or WAN loss. */
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    const unsigned arrivals=1800;unsigned received=0,completed=0,idr_due=UINT_MAX;
    uint64_t now=0,next_arrival=0,decode_done=UINT64_MAX;
    bool inflight=false;X4StreamAuLease lease={0};X4StreamAuView view={0};
    uint8_t held_byte=0;uint64_t intents=0;
    while(received<arrivals || inflight || snapshot(q).ready_entries){
        uint64_t event=received<arrivals?next_arrival:UINT64_MAX;
        if(decode_done<event)event=decode_done;
        if(event==UINT64_MAX)break;
        now=event;
        if(inflight && decode_done==now){assert(view.data[0]==held_byte && view.data[view.bytes-1]==held_byte);
            assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);inflight=false;decode_done=UINT64_MAX;++completed;}
        if(received<arrivals && next_arrival==now){
            size_t bytes=(received%29==0)?700000:1000+(received%7)*631;
            uint8_t input_byte=(uint8_t)(received%251);memset(payload,input_byte,bytes);
            X4StreamAuInput a=input(bytes,received+1,received==0 || received==idr_due);
            a.arrival_us=now;a.rtp_timestamp_valid=true;a.rtp_timestamp=(uint32_t)(received*1500);
            X4StreamAuOutcome out;X4StreamAuResult rc=x4_stream_au_queue_try_push(q,&a,now,&out);
            assert(rc>=0 && rc!=X4_STREAM_AU_BUSY && rc!=X4_STREAM_AU_STOPPED);
            if(out.request_keyframe){++intents;idr_due=received+2;}
            ++received;next_arrival=(uint64_t)received*1000000/60;
        }
        if(!inflight){X4StreamAuOutcome out;
            X4StreamAuResult rc=x4_stream_au_queue_try_take(q,now,&lease,&view,&out);
            if(out.request_keyframe){++intents;idr_due=received+1;}
            if(rc==X4_STREAM_AU_OK){inflight=true;held_byte=view.data[0];
                decode_done=now+((completed%23==22)?90000:20000);}
            else assert(rc==X4_STREAM_AU_EMPTY || rc==X4_STREAM_AU_DROPPED_LATE);
        }
        X4StreamAuStats s=snapshot(q);
        assert(s.resident_entries<=32 && s.resident_bytes<=8*1024*1024);
    }
    X4StreamAuStats s=snapshot(q);
    assert(received==arrivals && completed>600 && completed<arrivals);
    assert(s.request_intentions==intents && s.recovery_transitions==intents && intents>0);
    assert(s.dropped_busy==0 && s.evicted_non_idr>0 && s.dropped_wait_idr>0);
    printf("AU-only simulation: arrivals=%u decoded=%u local_IDR_intentions=%llu queue_lag_max_us=%llu producer_events_delayed_by_Decode=0 (RTP not modeled here)\n",
        received,completed,(unsigned long long)intents,(unsigned long long)s.lag_max_us);
    close_queue(q);
}
typedef struct { X4StreamAuQueue *queue;atomic_bool done;atomic_uint consumed; } ThreadStress;
static void *stress_producer(void *arg)
{
    ThreadStress *s=arg;bool need_idr=true;
    for(unsigned i=1;i<=5000;++i){size_t bytes=64+(i%127)*13;
        memset(payload,(int)(i%251),bytes);X4StreamAuInput a=input(bytes,i,need_idr||i%64==0);
        X4StreamAuOutcome out;X4StreamAuResult rc=x4_stream_au_queue_try_push(s->queue,&a,0,&out);
        assert(rc>=0 && rc!=X4_STREAM_AU_STOPPED);need_idr=out.waiting_idr;
        if(i%17==0)sched_yield();
    }
    atomic_store_explicit(&s->done,true,memory_order_release);return NULL;
}
static void *stress_consumer(void *arg)
{
    ThreadStress *s=arg;
    for(;;){X4StreamAuLease lease;X4StreamAuView v;
        X4StreamAuResult rc=x4_stream_au_queue_try_take(s->queue,0,&lease,&v,NULL);
        if(rc==X4_STREAM_AU_OK){uint8_t expected=(uint8_t)(v.identity%251);
            for(size_t i=0;i<v.bytes;++i)assert(v.data[i]==expected);
            assert(v.sps[0]==0x67 && v.pps[0]==0x68);
            sched_yield();
            assert(v.data[0]==expected && v.data[v.bytes-1]==expected);
            assert(x4_stream_au_queue_release(s->queue,lease)==X4_STREAM_AU_OK);
            atomic_fetch_add_explicit(&s->consumed,1,memory_order_relaxed);
        } else {
            assert(rc==X4_STREAM_AU_EMPTY || rc==X4_STREAM_AU_BUSY || rc==X4_STREAM_AU_DROPPED_WAIT_IDR);
            if(atomic_load_explicit(&s->done,memory_order_acquire)){
                X4StreamAuStats stats;
                if(x4_stream_au_queue_try_snapshot(s->queue,&stats)==X4_STREAM_AU_OK && !stats.ready_entries)break;
            }
            sched_yield();
        }
    }
    return NULL;
}
static void test_concurrent_publication_and_ownership(void)
{
    ThreadStress s={.queue=x4_stream_au_queue_create()};assert(s.queue);
    atomic_init(&s.done,false);atomic_init(&s.consumed,0);
    pthread_t producer,consumer;
    assert(pthread_create(&consumer,NULL,stress_consumer,&s)==0);
    assert(pthread_create(&producer,NULL,stress_producer,&s)==0);
    assert(pthread_join(producer,NULL)==0 && pthread_join(consumer,NULL)==0);
    X4StreamAuStats stats=snapshot(s.queue);
    assert(stats.taken==stats.released && stats.taken==atomic_load(&s.consumed));
    assert(!stats.fatal && stats.inflight_entries==0 && stats.admitted<=5000);
    close_queue(s.queue);
}
int main(void)
{
    payload=malloc(X4_STREAM_AU_MAX_BYTES);assert(payload);
    test_owned_spans_and_fifo();test_entries_include_inflight();
    test_byte_capacity_and_contiguous_wrap();test_lag_and_recovery();
    test_rtp_modular_lag();test_recovery_cause_fixed_per_episode();test_hooks_and_cancellation();test_empty_poll_does_not_touch_gate_or_histogram();test_invalid_limits();
    test_simulated_decode_20ms_with_90ms_spikes();test_concurrent_publication_and_ownership();free(payload);
    puts("PASS stream AU queue: owned variable payloads, 8MiB wrap, 32 including INFLIGHT, recovery, cancellation, 300ms and simulated 60AU/s");
    return 0;
}
