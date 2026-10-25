/* SPDX-License-Identifier: GPL-3.0-only */
/* Real streaming raw adapter, shared layout/ring and SPS parser. Module,
 * native decoder and direct-memory calls are the existing host doubles.
 * This is contract/cleanup coverage, not PS4 color, cadence or compatibility. */
#define X4_NATIVE_MOCK_STREAM_FAULTS 1
#define main x4_unused_legacy_native_fixture_main
#include "probe_native_mock_test.c"
#undef main
#include "../src/video/video_pipeline.h"
#define main x4_unused_sps_fixture_main
#include "stream_sps_test.c"
#undef main

static atomic_bool stream_cancel;
static const uint8_t slice_au[]={0,0,0,1,0x65,0x80};
static void *delayed_pointer;
static size_t delayed_bytes;
static unsigned delayed_calls;
static bool delayed_error;
static void *foreign_pointer;
static int foreign_kind;
/* The real CPU config module is linked. These scheduling doubles must remain
 * unused by raw_start_cpu: its CPU policy goes to native Query/Create, while
 * the production worker applies its own caller settings separately. */
static unsigned cpu_scheduler_calls;
OrbisPthread scePthreadSelf(void){++cpu_scheduler_calls;return pthread_self();}
int32_t scePthreadSetprio(OrbisPthread thread,int priority)
{(void)thread;(void)priority;++cpu_scheduler_calls;return 0;}
int32_t scePthreadSetaffinity(OrbisPthread thread,uint64_t mask)
{(void)thread;(void)mask;++cpu_scheduler_calls;return 0;}

static int32_t delayed_decode(void *d,const OrbisVideodec2InputData *in,
    OrbisVideodec2FrameBuffer *frame,OrbisVideodec2OutputInfo *out)
{
    (void)d;CHECK(in->auSize==sizeof(slice_au));++decode_calls;
    frame->isAccepted=true;
    if(!delayed_calls++) {delayed_pointer=frame->pFrameBuffer;delayed_bytes=frame->frameBufferSize;return 0;}
    OrbisVideodec2FrameBuffer earlier={.pFrameBuffer=delayed_pointer,.frameBufferSize=delayed_bytes};
    output_picture(&earlier,out);out->isErrorFrame=delayed_error;return 0;
}
static int32_t invalid_decode(void *d,const OrbisVideodec2InputData *in,
    OrbisVideodec2FrameBuffer *frame,OrbisVideodec2OutputInfo *out)
{
    int rc=decode(d,in,frame,out);if(rc)return rc;
    if(foreign_kind==1)out->pFrameBuffer=foreign_pointer;
    else if(foreign_kind==2)out->frameBufferSize=frame->frameBufferSize+32768;
    else if(foreign_kind==3)out->isErrorFrame=true;
    else if(foreign_kind==4){out->isErrorFrame=true;out->isValid=false;}
    else if(foreign_kind==5){out->isErrorFrame=true;out->pFrameBuffer=foreign_pointer;}
    return 0;
}
static int32_t failed_queue_close(void *queue){(void)queue;return -922;}
static int32_t positive_queue_close(void *queue){(void)queue;return 75;}
static size_t make_sps(uint8_t nal[512],unsigned profile,unsigned height)
{
    Fixture f=standard();f.profile=profile;f.constraints=profile==66?0xe0:0;
    f.width=height==720?1280:1920;f.height=height;f.crop_b=height==720?0:8;f.level=height==720?32:42;
    return fixture(nal,&f);
}
static void reset_stream(void)
{
    reset_fixture();atomic_store(&stream_cancel,false);valid_output=true;
    delayed_pointer=NULL;delayed_bytes=0;delayed_calls=0;delayed_error=false;foreign_kind=0;foreign_pointer=NULL;
}
static void cleanup(X4LiveVideo *v)
{
    CHECK(x4_live_video_stop(v)==0 && !v->state && !reserved_count());
    X4StreamVideoInfo info={0};x4_live_video_stream_info(v,&info);
    CHECK(info.close_result==0 && !info.retained);
}
static void cpu_query_create_contract(void)
{
    static const int priorities[]={700,480,480,256};
    static const uint64_t affinities[]={0x3f,0x3f,0x0f,0x0f};
    uint8_t sps[512];size_t n;
    for(unsigned cpu=X4_CPU_0;cpu<=X4_CPU_3;++cpu){
        n=make_sps(sps,100,1088);
        reset_stream();X4LiveVideo v={0};cpu_scheduler_calls=0;
        CHECK(!x4_live_video_stream_raw_start_cpu(&v,sps,n,&stream_cancel,cpu));
        CHECK(query_calls==1 && create_calls==1);
        CHECK(!memcmp(&stream_query_configs[0],&stream_create_configs[0],sizeof(seen_config)));
        CHECK(seen_config.cpuThreadPriority==priorities[cpu] && seen_config.cpuAffinityMask==affinities[cpu]);
        CHECK(v.stream_info.cpu_variant==cpu && v.stream_info.decoder_cpu_priority==priorities[cpu]);
        CHECK(v.stream_info.decoder_cpu_affinity==affinities[cpu]);
        CHECK(!v.stream_info.query_result && !v.stream_info.create_result && !cpu_scheduler_calls);
        printf("PASS real raw_start_cpu CPU-%u Query/Create priority=%d affinity=0x%llx query_rc=0 create_rc=0\n",
            cpu,priorities[cpu],(unsigned long long)affinities[cpu]);
        cleanup(&v);
        /* Each retry receives the same selected CPU policy; native failures
         * are exposed unchanged, with no silent CPU-0 fallback. */
        n=make_sps(sps,66,720); /* Includes the eligible third legacy attempt. */
        for(unsigned failure=0;failure<2;++failure){
            reset_stream();v=(X4LiveVideo){0};cpu_scheduler_calls=0;
            if(failure)create_rc=-202;else query_rc=-101;
            CHECK(x4_live_video_stream_raw_start_cpu(&v,sps,n,&stream_cancel,cpu)==(failure?-202:-101));
            CHECK(query_calls==3 && create_calls==(failure?3u:0u));
            for(unsigned attempt=0;attempt<3;++attempt){
                CHECK(stream_query_configs[attempt].cpuThreadPriority==priorities[cpu]);
                CHECK(stream_query_configs[attempt].cpuAffinityMask==affinities[cpu]);
                if(failure)CHECK(!memcmp(&stream_query_configs[attempt],&stream_create_configs[attempt],sizeof(seen_config)));
            }
            CHECK(!v.state && !reserved_count() && !cpu_scheduler_calls);
            CHECK(v.stream_info.cpu_variant==cpu && v.stream_info.decoder_cpu_priority==priorities[cpu]);
            CHECK(v.stream_info.decoder_cpu_affinity==affinities[cpu]);
            CHECK(failure?v.stream_info.create_result==-202:v.stream_info.query_result==-101);
            printf("PASS real raw_start_cpu CPU-%u %s failure=%d all3 configs preserve priority=%d affinity=0x%llx\n",
                cpu,failure?"Create":"Query",failure?-202:-101,priorities[cpu],(unsigned long long)affinities[cpu]);
        }
    }
    reset_stream();X4LiveVideo invalid={0};
    CHECK(x4_live_video_stream_raw_start_cpu(&invalid,sps,n,&stream_cancel,X4_CPU_3+1)==-7);
    CHECK(!query_calls && !create_calls && !invalid.state && !reserved_count());
    puts("PASS real raw_start_cpu all CPU variants: identical native Query/Create CPU fields, exact returns and invalid selector rejected");
}
static void profiles_and_layout(void)
{
    for(unsigned size=0;size<2;++size)for(unsigned profile=0;profile<3;++profile){
        static const unsigned profiles[]={66,77,100};
        unsigned height=size?1088:720;uint8_t sps[512];size_t n=make_sps(sps,profiles[profile],height);
        reset_stream();X4LiveVideo v={0};X4Trace *trace=(X4Trace *)(uintptr_t)0x1234;
        v.trace=trace;v.trace_output_next=42;v.trace_decoder_epoch=5;
        CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
        CHECK(v.trace==trace && v.trace_output_next==42 && v.trace_decoder_epoch==6);
        CHECK(query_calls==1 && create_calls==1 && !memcmp(&seen_config,&created_config,sizeof(seen_config)));
        CHECK(seen_config.profile==profiles[profile] && seen_config.maxLevel==(size?42:32));
        CHECK(seen_config.maxFrameWidth==(size?1920:1280) && seen_config.maxFrameHeight==height);
        CHECK(seen_config.maxDpbFrameCount==4 && seen_config.decodePipelineDepth==1);
        VideoState *state=v.state;
        CHECK(!state->staging && !state->parallel_enabled && copy_helper_count(state)==0);
        for(unsigned slot=0;slot<4;++slot)CHECK(reservations[state->output[slot].offset].type==0);
        X4StreamVideoInfo info={0};x4_live_video_stream_info(&v,&info);
        CHECK(info.planned_native_bytes && info.planned_native_bytes<=128u*1024u*1024u);
        CHECK(!info.query_result && !info.create_result && !info.retained);
        for(unsigned slot=0;slot<4;++slot){
            X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(&v,slot,slice_au,sizeof(slice_au),999,false,&out));
            CHECK(out.valid && out.accepted && out.slot==slot && out.layout_verified && out.completed_output && !out.ownership_uncertain && out.frame_class==X4_STREAM_FRAME_OK);
            CHECK(out.output_id==43u+slot && out.decode_us==1 && out.native_result==0);
            CHECK(out.layout.visible_width==(size?1920:1280) && out.layout.visible_height==(size?1080:720));
            CHECK(out.layout.storage_height==height && out.layout.uv_plane_offset==(size_t)(size?1920:1280)*height);
            CHECK(out.pitch==(size?1920:1280) && out.owned_bytes>=out.returned_bytes);
        }
        CHECK(v.copy_calls==0 && v.frames==0 && v.convert_us==0);cleanup(&v);
    }
    puts("PASS raw streaming CB/Main/High 720/1088: exact Query/Create SPS contract, WB_ONION, no copy/RGB staging, trace identity");
}
static void admission_and_start_failures(void)
{
    uint8_t sps[512];size_t n=make_sps(sps,66,720);X4LiveVideo v={0};
    reset_stream();query_rc=-101;CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-101);
    CHECK(query_calls==3 && !create_calls && !v.state && !reserved_count() && v.stream_info.query_result==-101);
    CHECK(v.stream_info.attempt_count==3 && v.stream_info.fallback_used);
    reset_stream();v=(X4LiveVideo){0};create_rc=-202;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-202);
    CHECK(query_calls==3 && create_calls==3 && !v.state && !reserved_count() && v.stream_info.create_result==-202);
    CHECK(v.stream_info.attempt_count==3 && v.stream_info.fallback_used);
    reset_stream();v=(X4LiveVideo){0};query_cpu_bytes=query_gpu_bytes=64u*1024u*1024u;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)<0);
    CHECK(query_calls==3 && !create_calls && !v.state && !reserved_count());
    reset_stream();v=(X4LiveVideo){0};reject_cacheable_output_allocate=true;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-818);
    CHECK(!create_calls && !v.state && !reserved_count());
    reset_stream();v=(X4LiveVideo){0};sps[4]|=0x80;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)<0);
    CHECK(!query_calls && !create_calls && !v.state && !reserved_count());
    reset_stream();v=(X4LiveVideo){0};
    Fixture f=standard();f.matrix=1;f.full=1;n=fixture(sps,&f);
    CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    X4StreamVideoInfo admitted={0};x4_live_video_stream_info(&v,&admitted);
    CHECK(admitted.sps.matrix_coefficients==1 && admitted.sps.full_range==1 && admitted.sps.profile_idc==66);
    CHECK(admitted.sps.visible_width==1280 && admitted.sps.visible_height==720);
    CHECK(!admitted.query_result && !admitted.create_result && admitted.sps_diagnostics.syntax_complete);
    CHECK(query_calls==1 && create_calls==1 && !decode_calls);cleanup(&v);
    reset_stream();v=(X4LiveVideo){0};n=make_sps(sps,100,1088);
    /* Crop syntax failure is observed before any native calls or allocations. */
    f=standard();f.crop_l=1280;n=fixture(sps,&f);
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==X4_SPS_CROP);
    CHECK(v.stream_info.sps.crop_left==1280 && !v.stream_info.sps_diagnostics.syntax_complete);
    CHECK(v.stream_info.query_result==INT32_MIN && v.stream_info.create_result==INT32_MIN);
    CHECK(!query_calls && !create_calls && !next_reservation && !v.state);
    puts("PASS raw streaming typed SPS rejection, bounded Query/Create retry, allocation/budget failures and cleanup");
    puts("PASS BT.709/full-range SPS is admitted natively; partial invalid-crop metadata remains observable before native calls");
}
static int query_first_error(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)c;return call==1?-1101:0;}
static int create_first_error(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)c;return call==1?-2201:0;}
static int query_first_two_errors(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)c;return call<=2?-(1100+(int)call):0;}
static int query_budget_once(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)c;query_cpu_bytes=query_gpu_bytes=call==1?64u*1024u*1024u:16384;return 0;}
static int query_cancel_error(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)c;if(call==1){atomic_store(&stream_cancel,true);return -3301;}return 0;}
static int create_cancel_error(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)c;if(call==1){atomic_store(&stream_cancel,true);return -3302;}return 0;}
static void fallback_attempts_and_exclusions(void)
{
    uint8_t sps[512];Fixture f=standard();f.profile=100;f.constraints=0;f.level=99;f.dpb=8;
    size_t n=fixture(sps,&f);
    for(unsigned cause=0;cause<2;++cause){
        reset_stream();X4LiveVideo v={0};
        if(cause)stream_create_fault=create_first_error;else stream_query_fault=query_first_error;
        CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
        const X4StreamVideoInfo *i=&v.stream_info;
        CHECK(i->attempt_count==2 && i->active_attempt==1 && i->fallback_used);
        CHECK(query_calls==2 && create_calls==(cause?2u:1u));
        CHECK(i->attempts[0].profile==100 && i->attempts[0].level==99 && i->attempts[0].dpb==8);
        CHECK(!i->attempts[0].level_known && i->attempts[0].dpb_level_cap==16);
        CHECK(i->attempts[0].query==(cause?0:-1101));
        CHECK(i->attempts[0].create==(cause?-2201:INT32_MIN) && i->attempts[0].cleanup==0);
        CHECK(i->attempts[0].failure_reason==X4_STREAM_POLICY_NATIVE);
        CHECK(!strcmp(i->attempts[0].failure_stage,cause?"CREATE DECODER":"QUERY DECODER"));
        CHECK(!i->attempts[1].query && !i->attempts[1].create && i->attempts[1].fallback);
        CHECK(i->attempts[1].profile==100 && i->attempts[1].level==32 && i->attempts[1].dpb==5);
        CHECK(i->attempts[1].reason==X4_STREAM_POLICY_LEVEL_LIMITED);
        CHECK(stream_query_configs[1].profile==100 && stream_query_configs[1].maxLevel==32 && stream_query_configs[1].maxDpbFrameCount==5);
        CHECK(stream_query_configs[1].maxFrameWidth==1280 && stream_query_configs[1].maxFrameHeight==720);
        CHECK(!memcmp(&stream_query_configs[1],&stream_create_configs[cause?1:0],sizeof(seen_config)));
        CHECK(!i->retained && reserved_count()==10);
        X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(&v,0,slice_au,sizeof(slice_au),1,false,&out));
        CHECK(out.valid && out.accepted && v.decoded_frames==1);cleanup(&v);
    }
    /* Legacy is a third attempt only, preserving geometry and Query/Create. */
    reset_stream();X4LiveVideo v={0};stream_query_fault=query_first_two_errors;
    CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(query_calls==3 && create_calls==1 && v.stream_info.attempt_count==3);
    CHECK(v.stream_info.attempts[2].kind==X4_STREAM_ATTEMPT_LEGACY);
    CHECK(seen_config.profile==66 && seen_config.maxLevel==31 && seen_config.maxDpbFrameCount==4);
    CHECK(!memcmp(&seen_config,&created_config,sizeof(seen_config)));cleanup(&v);
    /* All three native failures preserve their individual outcomes. */
    reset_stream();v=(X4LiveVideo){0};create_rc=-2202;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-2202);
    CHECK(query_calls==3 && create_calls==3 && !v.state && !reserved_count());
    for(unsigned a=0;a<3;++a)CHECK(v.stream_info.attempts[a].create==-2202 && v.stream_info.attempts[a].cleanup==0);
    /* Query budget excess is retryable, allocation failure is not. */
    reset_stream();v=(X4LiveVideo){0};stream_query_fault=query_budget_once;
    CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(query_calls==2 && create_calls==1 && v.stream_info.attempts[0].failure_reason==X4_STREAM_POLICY_QUERY_BUDGET);
    CHECK(!strcmp(v.stream_info.attempts[0].failure_stage,"PRESUPUESTO DECODER"));
    CHECK(v.stream_info.attempts[0].cleanup==0);cleanup(&v);
    reset_stream();v=(X4LiveVideo){0};reject_cacheable_output_allocate=true;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-818);
    CHECK(query_calls==1 && !create_calls && v.stream_info.attempt_count==1 && !v.state && !reserved_count());
    CHECK(!strcmp(v.stream_info.attempts[0].failure_stage,"MEMORIA SALIDA"));
    reset_stream();v=(X4LiveVideo){0};stream_create_success_null=true;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-7);
    CHECK(query_calls==1 && create_calls==1 && !delete_calls && v.state && reserved_count()==10);
    CHECK(v.stream_info.creation_uncertain && v.stream_info.retained && v.stream_info.attempt_count==1);
    CHECK(!strcmp(v.stream_info.attempts[0].failure_stage,"HANDLE DECODER INVALIDO"));
    CHECK(x4_live_video_stop(&v)==-7 && v.state && reserved_count()==10 && !release_calls);
    /* Host fixture repair only: production has no handle or retirement proof
     * and must retain these allocations rather than infer successful close. */
    v.stream_info.creation_uncertain=false;cleanup(&v);
    /* Cancellation at entry or between attempts cannot dispatch another Query. */
    reset_stream();v=(X4LiveVideo){0};atomic_store(&stream_cancel,true);
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==X4_STREAM_VIDEO_CANCELLED);
    CHECK(!query_calls && !create_calls && !v.stream_info.attempt_count && !v.state && !reserved_count());
    for(unsigned cause=0;cause<2;++cause){
        reset_stream();v=(X4LiveVideo){0};
        if(cause)stream_create_fault=create_cancel_error;else stream_query_fault=query_cancel_error;
        CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==X4_STREAM_VIDEO_CANCELLED);
        CHECK(query_calls==1 && create_calls==(cause?1u:0u) && v.stream_info.attempt_count==1 && !v.state && !reserved_count());
        CHECK(v.stream_info.attempts[0].cleanup==0 && !v.stream_info.fallback_used);
    }
    /* Unconfirmed cleanup retains all ownership and forbids retry. */
    reset_stream();v=(X4LiveVideo){0};stream_query_fault=query_first_error;release_rc=-923;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-1101);
    CHECK(query_calls==1 && v.stream_info.attempt_count==1 && v.stream_info.retained && v.state && reserved_count()==1);
    CHECK(v.stream_info.attempts[0].cleanup==-923);release_rc=0;cleanup(&v);
    reset_stream();v=(X4LiveVideo){0};f.width=640;f.height=480;n=fixture(sps,&f);query_rc=-1105;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-1105);
    CHECK(query_calls==1 && !create_calls && !v.state && !reserved_count());
    CHECK(v.stream_info.attempts[1].skipped && v.stream_info.attempts[2].skipped);
    puts("PASS three bounded attempts: SPS, geometry level, legacy720; retry Query budget, never retry allocation/cancel/unsafe cleanup");
}
static void dpb_policy(void)
{
    uint8_t sps[512];Fixture f=standard();f.refs=1;f.dpb=1;size_t n=fixture(sps,&f);
    reset_stream();X4LiveVideo v={0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(v.stream_info.sps.max_dec_frame_buffering==1 && seen_config.maxDpbFrameCount==4);
    CHECK(v.stream_info.attempts[0].dpb_minimum==4 && v.stream_info.attempts[0].reason==X4_STREAM_POLICY_FLOOR);cleanup(&v);
    f.refs=4;f.dpb=1;f.level=32;n=fixture(sps,&f);reset_stream();v=(X4LiveVideo){0};
    CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(seen_config.maxDpbFrameCount==5 && v.stream_info.attempts[0].dpb_minimum==5);cleanup(&v);
    f.width=1920;f.height=1088;f.crop_b=8;f.level=42;n=fixture(sps,&f);
    reset_stream();v=(X4LiveVideo){0};CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==X4_SPS_LEVEL_DPB);
    CHECK(!query_calls && !create_calls && !v.state && !reserved_count());
    for(unsigned a=0;a<2;++a)CHECK(v.stream_info.attempts[a].skipped && v.stream_info.attempts[a].dpb_level_cap==4 && v.stream_info.attempts[a].dpb_minimum==5 && v.stream_info.attempts[a].reason==X4_STREAM_POLICY_LEVEL_CONFLICT && v.stream_info.attempts[a].query==INT32_MIN);
    CHECK(v.stream_info.attempts[2].skipped && v.stream_info.attempts[2].reason==X4_STREAM_POLICY_INELIGIBLE_GEOMETRY);
    f.width=1280;f.height=720;f.crop_b=0;f.level=10;n=fixture(sps,&f);
    reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(v.stream_info.attempts[0].skipped && query_calls==1 && seen_config.maxLevel==32 && seen_config.maxDpbFrameCount==5);cleanup(&v);
    f.level=99;n=fixture(sps,&f);reset_stream();v=(X4LiveVideo){0};query_rc=-101;
    CHECK(x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel)==-101);
    CHECK(query_calls==2 && v.stream_info.attempts[2].skipped && v.stream_info.attempts[2].reason==X4_STREAM_POLICY_REF_CONFLICT);
    CHECK(!v.state && !reserved_count());
    puts("PASS DPB refs+1/floor4, known-level clamp/skip, unknown16 bound, no legacy below refs");
}
static void alternative_geometry_and_crop(void)
{
    uint8_t sps[512];Fixture f=standard();f.width=1280;f.height=736;f.crop_b=16;
    size_t n=fixture(sps,&f);reset_stream();X4LiveVideo v={0};
    CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(seen_config.maxFrameWidth==1280 && seen_config.maxFrameHeight==736 && v.height==720);
    X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(&v,0,slice_au,sizeof(slice_au),1,false,&out));
    CHECK(out.valid && out.layout_verified && out.native_width==1280 && out.native_height==736 && out.pitch==1280);
    CHECK(out.layout.visible_width==1280 && out.layout.visible_height==720 && out.layout.storage_height==736);
    CHECK(out.layout.uv_plane_offset==(size_t)1280*736 && v.stream_info.geometry_warnings==1);
    CHECK(out.returned_bytes==1413120u && out.owned_bytes>=out.returned_bytes);
    cleanup(&v);
    /* A native geometry that differs from the SPS is retained as an observed
     * result and safely scaled, provided the returned allocation bounds fit. */
    reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    output_height_override=720;out=(X4ProbeNativeOutput){0};
    CHECK(!x4_live_video_stream_raw_decode(&v,0,slice_au,sizeof(slice_au),2,false,&out));
    CHECK(out.valid && out.native_height==720 && out.layout.visible_height==720 && v.stream_info.geometry_warnings==1);cleanup(&v);
    puts("PASS raw streaming 1280x736/crop-bottom16 retains actual storage, visible720 and bounded alternate native geometry");
}
static void delayed_and_foreign_outputs(void)
{
    uint8_t sps[512];size_t n=make_sps(sps,66,720);X4LiveVideo v={0};reset_stream();
    CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    ((VideoState*)v.state)->Decode=delayed_decode;
    X4VideoPipelineRing ring;CHECK(!x4_video_pipeline_ring_init(&ring,1));
    X4VideoPipelineLease first,second,returned;
    CHECK(!x4_video_pipeline_acquire_decode(&ring,&first,NULL));
    X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(&v,first.slot,slice_au,sizeof(slice_au),1,false,&out));
    CHECK(!out.valid && out.accepted && ring.slots[first.slot].state==X4_PIPELINE_DECODING);
    CHECK(!x4_video_pipeline_acquire_decode(&ring,&second,NULL) && second.slot!=first.slot);
    CHECK(!x4_live_video_stream_raw_decode(&v,second.slot,slice_au,sizeof(slice_au),2,false,&out));
    CHECK(out.valid && out.accepted && out.slot==first.slot && out.slot!=second.slot);
    CHECK(!x4_video_pipeline_decoding_lease(&ring,out.slot,&returned));
    CHECK(!x4_video_pipeline_publish_ready(&ring,returned,out.output_id,out.decode_end_us));
    CHECK(ring.slots[second.slot].state==X4_PIPELINE_DECODING);
    CHECK(!x4_video_pipeline_take_ready(&ring,&returned,NULL));
    CHECK(!x4_video_pipeline_release_conversion(&ring,returned));
    cleanup(&v);CHECK(!x4_video_pipeline_abort_decode(&ring,second));
    reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    ((VideoState*)v.state)->Decode=delayed_decode;delayed_error=true;
    CHECK(!x4_video_pipeline_ring_init(&ring,2));
    CHECK(!x4_video_pipeline_acquire_decode(&ring,&first,NULL));
    CHECK(!x4_live_video_stream_raw_decode(&v,first.slot,slice_au,sizeof(slice_au),1,false,&out));
    CHECK(!out.valid && out.accepted && !out.completed_output);
    CHECK(!x4_video_pipeline_acquire_decode(&ring,&second,NULL));
    CHECK(!x4_live_video_stream_raw_decode(&v,second.slot,slice_au,sizeof(slice_au),2,false,&out));
    CHECK(out.frame_class==X4_STREAM_FRAME_BAD_FRAME && out.completed_output && out.slot==first.slot && out.slot!=second.slot);
    CHECK(!x4_video_pipeline_decoding_lease(&ring,out.slot,&returned));
    CHECK(!x4_video_pipeline_abort_decode(&ring,returned));
    CHECK(ring.slots[first.slot].state==X4_PIPELINE_FREE && ring.slots[second.slot].state==X4_PIPELINE_DECODING);
    cleanup(&v);CHECK(!x4_video_pipeline_abort_decode(&ring,second));
    for(int kind=1;kind<=5;++kind){
        reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
        foreign_kind=kind;CHECK(posix_memalign(&foreign_pointer,16,16)==0);
        ((VideoState*)v.state)->Decode=invalid_decode;out=(X4ProbeNativeOutput){0};
        int rc=x4_live_video_stream_raw_decode(&v,0,slice_au,sizeof(slice_au),1,false,&out);
        if(kind<3)CHECK(rc<0 && out.frame_class==X4_STREAM_FRAME_FATAL && out.ownership_uncertain && !out.completed_output);
        else if(kind==3)CHECK(!rc && out.frame_class==X4_STREAM_FRAME_BAD_FRAME && out.completed_output && out.slot==0 && !out.ownership_uncertain && !v.error);
        else CHECK(!rc && out.frame_class==X4_STREAM_FRAME_RECOVERABLE_RESTART && out.ownership_uncertain && !out.completed_output && !v.error);
        if(kind!=3)CHECK(out.control_result==-7 && out.native_width==1280 && out.native_height==720 && out.pitch==1280 && !out.layout_verified);
        CHECK(!out.valid && v.decoded_frames==0);free(foreign_pointer);foreign_pointer=NULL;cleanup(&v);
    }
    puts("PASS delayed raw output identifies original lease; foreign pointer, oversized return and error frame never publish");
}
static void changes_and_native_errors(void)
{
    uint8_t sps[512],changed[512];size_t n=make_sps(sps,66,720),m=make_sps(changed,100,1088);
    reset_stream();X4LiveVideo v={0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    X4ProbeNativeOutput out={0};
    CHECK(!x4_live_video_stream_raw_decode(&v,0,changed,m,0,false,&out) && !decode_calls);
    CHECK(out.frame_class==X4_STREAM_FRAME_RECONFIGURE && out.control_result==X4_SPS_CHANGED && !out.native_result && !out.accepted && !out.ownership_uncertain && !v.error);cleanup(&v);
    reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    atomic_store(&stream_cancel,true);CHECK(x4_live_video_stream_raw_decode(&v,0,slice_au,sizeof(slice_au),0,false,&out)==3 && !decode_calls);cleanup(&v);
    reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    decode_rc=-303;CHECK(!x4_live_video_stream_raw_decode(&v,0,slice_au,sizeof(slice_au),0,false,&out));
    CHECK(out.native_result==-303 && !out.valid && v.state && reserved_count() && out.frame_class==X4_STREAM_FRAME_RECOVERABLE_RESTART && out.ownership_uncertain && !v.error);cleanup(&v);
    reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    flush_rc=-404;CHECK(!x4_live_video_stream_raw_decode(&v,0,NULL,0,0,true,&out) && flush_calls==1);
    CHECK(out.frame_class==X4_STREAM_FRAME_RECOVERABLE_RESTART && out.native_result==-404 && out.ownership_uncertain && !v.error);cleanup(&v);
    for(unsigned kind=0;kind<3;++kind){
        reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
        VideoState *state=v.state;
        if(kind==0)delete_rc=-921;else if(kind==1)state->ReleaseComputeQueue=failed_queue_close;else release_rc=-923;
        CHECK(x4_live_video_stop(&v)<0 && v.state && reserved_count());
        X4StreamVideoInfo info={0};x4_live_video_stream_info(&v,&info);CHECK(info.retained);
        if(kind<2)CHECK(reserved_count()==10);
        /* Host-only recovery after an injected error; production retains this
         * owner and must not infer that an unconfirmed close is safe. */
        delete_rc=release_rc=0;state->ReleaseComputeQueue=release_queue;cleanup(&v);
    }
    puts("PASS changed SPS rejects before Decode; cancellation/native failures preserve result and native ownership on close failure");
}
static int32_t bad_then_good_decode(void *d,const OrbisVideodec2InputData *in,
    OrbisVideodec2FrameBuffer *frame,OrbisVideodec2OutputInfo *out)
{
    int rc=decode(d,in,frame,out);if(rc)return rc;
    if(decode_calls==2)out->isErrorFrame=true;
    return 0;
}
static int32_t accepted_error_decode(void *d,const OrbisVideodec2InputData *in,
    OrbisVideodec2FrameBuffer *frame,OrbisVideodec2OutputInfo *out)
{(void)d;(void)in;(void)out;++decode_calls;frame->isAccepted=true;return -303;}
static void damaged_frame_and_restart_ownership(void)
{
    uint8_t sps[512];size_t n=make_sps(sps,66,720);reset_stream();X4LiveVideo v={0};
    CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    ((VideoState*)v.state)->Decode=bad_then_good_decode;
    X4VideoPipelineRing ring;CHECK(!x4_video_pipeline_ring_init(&ring,7));
    for(unsigned call=0;call<3;++call){
        X4VideoPipelineLease offered,completed;CHECK(!x4_video_pipeline_acquire_decode(&ring,&offered,NULL));
        X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(&v,offered.slot,slice_au,sizeof(slice_au),call,false,&out));
        CHECK(out.completed_output && out.slot==offered.slot && !out.ownership_uncertain && !v.error);
        CHECK(!x4_video_pipeline_decoding_lease(&ring,out.slot,&completed));
        if(call==1){
            CHECK(out.frame_class==X4_STREAM_FRAME_BAD_FRAME && !out.valid);
            CHECK(!x4_video_pipeline_abort_decode(&ring,completed));
        }else{
            CHECK(out.frame_class==X4_STREAM_FRAME_OK && out.valid);
            CHECK(!x4_video_pipeline_publish_ready(&ring,completed,out.output_id,out.decode_end_us));
            CHECK(!x4_video_pipeline_take_ready(&ring,&completed,NULL));
            CHECK(!x4_video_pipeline_release_conversion(&ring,completed));
        }
    }
    CHECK(v.decode_calls==3 && v.decoded_frames==2);cleanup(&v);
    /* A result with uncertain ownership never releases an accepted lease.
     * Delete failure retains every reservation; only Delete==0 plus epoch
     * reset permits reuse. Native error code names are not guessed. */
    reset_stream();v=(X4LiveVideo){0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(!x4_video_pipeline_ring_init(&ring,9));X4VideoPipelineLease lease;
    CHECK(!x4_video_pipeline_acquire_decode(&ring,&lease,NULL));
    ((VideoState*)v.state)->Decode=accepted_error_decode;X4ProbeNativeOutput out={0};
    CHECK(!x4_live_video_stream_raw_decode(&v,lease.slot,slice_au,sizeof(slice_au),1,false,&out));
    CHECK(out.frame_class==X4_STREAM_FRAME_RECOVERABLE_RESTART && out.ownership_uncertain && out.accepted);
    CHECK(ring.slots[lease.slot].state==X4_PIPELINE_DECODING && reserved_count()==10);
    delete_rc=-921;CHECK(x4_live_video_stop(&v)==-921 && v.state && reserved_count()==10);
    CHECK(ring.slots[lease.slot].state==X4_PIPELINE_DECODING);
    delete_rc=0;cleanup(&v);CHECK(!x4_video_pipeline_ring_init(&ring,10));
    decode_rc=0;CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
    CHECK(v.decode_calls==1 && v.trace_decode_next==1 && v.trace_decoder_epoch==2);
    valid_output=false;CHECK(!x4_live_video_stream_raw_decode(&v,0,slice_au,sizeof(slice_au),2,false,&out));
    CHECK(!out.valid && out.frame_class==X4_STREAM_FRAME_OK && v.no_picture_calls==1 && v.decode_calls==2);
    valid_output=true;CHECK(!x4_live_video_stream_raw_decode(&v,1,slice_au,sizeof(slice_au),3,false,&out));
    CHECK(out.valid && v.decode_calls==3 && v.decoded_frames==1);cleanup(&v);
    puts("PASS good/damaged/good output, exact damaged lease, unknown native failure retains until Delete, counters survive restart");
}
static void positive_close_errors_retain(void)
{
    uint8_t sps[512];size_t n=make_sps(sps,66,720);
    for(unsigned kind=0;kind<3;++kind){
        reset_stream();X4LiveVideo v={0};CHECK(!x4_live_video_stream_raw_start(&v,sps,n,&stream_cancel));
        VideoState *state=v.state;void *decoder=state->decoder,*queue=state->queue;
        unsigned before=reserved_count();CHECK(before==10);
        if(kind==0)delete_rc=74;
        else if(kind==1)state->ReleaseComputeQueue=positive_queue_close;
        else release_rc=76;
        int rc=x4_live_video_stop(&v);CHECK(rc==-(74+(int)kind));
        CHECK(v.state==state && reserved_count()==before);
        X4StreamVideoInfo info={0};x4_live_video_stream_info(&v,&info);CHECK(info.retained);
        if(kind==0){CHECK(state->decoder==decoder && state->queue==queue);CHECK(info.delete_decoder_result==74 && !release_calls);}
        if(kind==1){CHECK(!state->decoder && state->queue==queue);CHECK(info.release_queue_result==75 && !release_calls);}
        if(kind==2){CHECK(!state->decoder && !state->queue);CHECK(!state->output[0].p && state->output[0].allocated && release_calls==1);}
        /* Fixture-only recovery to avoid leaks. Production never treats an
         * unknown close outcome as authorization to restart/reuse resources. */
        delete_rc=release_rc=0;state->ReleaseComputeQueue=release_queue;cleanup(&v);
    }
    puts("PASS positive Delete/queue-release/direct-release results normalize to errors and retain every reservation");
}
int main(void)
{
    cpu_query_create_contract();profiles_and_layout();admission_and_start_failures();fallback_attempts_and_exclusions();dpb_policy();alternative_geometry_and_crop();
    delayed_and_foreign_outputs();changes_and_native_errors();
    damaged_frame_and_restart_ownership();positive_close_errors_retain();
    reset_fixture();puts("PASS real stream raw adapter HOST MOCK; no PS4 compatibility, color or throughput claim");return 0;
}
