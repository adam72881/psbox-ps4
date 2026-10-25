/* SPDX-License-Identifier: GPL-3.0-only */
/* Production native adapter + production ported ledger. HOST decoder/direct
 * memory doubles verify ownership only, never firmware throughput or color. */
#define X4_NATIVE_MOCK_STREAM_FAULTS 1
#define main x4_unused_probe_native_main
#include "probe_native_mock_test.c"
#undef main
#define main x4_unused_sps_main
#include "stream_sps_test.c"
#undef main
#include "../src/video/video_pipeline.h"

static atomic_bool cancel_depth;
static const uint8_t depth_au[]={0,0,0,1,0x65,0x80};
OrbisPthread scePthreadSelf(void){return pthread_self();}
int32_t scePthreadSetprio(OrbisPthread t,int p){(void)t;(void)p;return 0;}
int32_t scePthreadSetaffinity(OrbisPthread t,uint64_t m){(void)t;(void)m;return 0;}

static void *pending_native[2];
static size_t pending_sizes[2];
static unsigned pending_count, delayed_returns, native_delay, damaged_call;
static void *seen_inputs[4];
static uint64_t seen_inputs_call[4];
static uint8_t seen_input_bytes[4];
static bool native_fail;
static int32_t depth_delayed_decode(void *d,const OrbisVideodec2InputData *in,
    OrbisVideodec2FrameBuffer *f,OrbisVideodec2OutputInfo *o)
{
    (void)d;CHECK(in->pAuData && in->auSize==sizeof(depth_au));
    ++decode_calls;++delayed_returns;
    /* The real caller must not overwrite an input before three subsequent
     * Decode returns. At the fourth later call it may offer that storage. */
    unsigned found=4;
    for(unsigned i=0;i<4;++i)if(seen_inputs[i]==in->pAuData)found=i;
    if(found<4)CHECK(delayed_returns-seen_inputs_call[found]>=4);
    else for(unsigned i=0;i<4;++i)if(!seen_inputs[i]){found=i;break;}
    CHECK(found<4);
    for(unsigned i=0;i<4;++i)if(seen_inputs[i]&&i!=found&&
        delayed_returns-seen_inputs_call[i]<=3)
        CHECK(((const uint8_t *)seen_inputs[i])[5]==seen_input_bytes[i]);
    seen_inputs[found]=(void *)in->pAuData;seen_inputs_call[found]=delayed_returns;
    seen_input_bytes[found]=((const uint8_t *)in->pAuData)[5];
    /* Deliberately FALSE, even while native retains its supplied output. */
    f->isAccepted=false;
    if(native_fail)return -730;
    if(pending_count==native_delay){
        OrbisVideodec2FrameBuffer prior={.pFrameBuffer=pending_native[0],.frameBufferSize=pending_sizes[0]};
        output_picture(&prior,o);o->isErrorFrame=damaged_call==delayed_returns;
        for(unsigned i=1;i<pending_count;++i){pending_native[i-1]=pending_native[i];pending_sizes[i-1]=pending_sizes[i];}
        --pending_count;
    }
    CHECK(pending_count<2);
    pending_native[pending_count]=f->pFrameBuffer;pending_sizes[pending_count]=f->frameBufferSize;++pending_count;
    return 0;
}
static void reset_depth(void)
{
    reset_fixture();atomic_store(&cancel_depth,false);valid_output=true;
    memset(pending_native,0,sizeof(pending_native));memset(pending_sizes,0,sizeof(pending_sizes));
    memset(seen_inputs,0,sizeof(seen_inputs));memset(seen_inputs_call,0,sizeof(seen_inputs_call));
    memset(seen_input_bytes,0,sizeof(seen_input_bytes));
    pending_count=delayed_returns=damaged_call=0;native_delay=1;native_fail=false;
}
static size_t depth_sps(uint8_t sps[512],bool hd)
{
    Fixture f=standard();f.profile=100;f.constraints=0;f.width=hd?1920:1280;
    f.height=hd?1088:720;f.crop_b=hd?8:0;f.level=hd?42:32;return fixture(sps,&f);
}
static void depth_start(X4LiveVideo *v,unsigned depth,bool hd)
{
    uint8_t sps[512];size_t n=depth_sps(sps,hd);
    CHECK(!x4_live_video_stream_raw_start_cpu_depth(v,sps,n,&cancel_depth,X4_CPU_0,depth));
    CHECK(v->stream_info.requested_depth==depth && v->stream_info.active_depth==depth);
    CHECK(seen_config.decodePipelineDepth==(int)depth);
    CHECK(!memcmp(&seen_config,&created_config,sizeof(seen_config)));
    CHECK(v->stream_info.input_count==(depth==2?4u:2u));
    CHECK(v->stream_info.output_count==(depth==2?5u:4u));
    CHECK(!x4_live_video_stream_output_held_mask(v));
    ((VideoState *)v->state)->Decode=depth_delayed_decode;
}
static void depth_close(X4LiveVideo *v)
{
    CHECK(!x4_live_video_stop(v) && !v->state && !reserved_count());
    X4StreamVideoInfo i={0};x4_live_video_stream_info(v,&i);
    CHECK(!i.retained && !i.close_result && !i.held_output_mask && !i.live_input_count);
    CHECK(!flush_calls); /* Streaming intentionally retires with Delete. */
}
static void ledger_retirement_exact(void)
{
    X4DecoderDepthLedger l;CHECK(!x4_decoder_depth_init(&l,2));
    CHECK(l.input_count==4 && l.output_count==5);
    for(unsigned n=0;n<4;++n){unsigned slot=9;CHECK(!x4_decoder_depth_input_acquire(&l,&slot)&&slot==n);
        CHECK(l.input_live[0]);x4_decoder_depth_decode_returned(&l);
        CHECK(l.input_live[0]==(n<3));}
    unsigned slot=9;CHECK(!x4_decoder_depth_input_acquire(&l,&slot)&&slot==0);
    CHECK(!x4_decoder_depth_output_offer(&l,0));CHECK(x4_decoder_depth_output_offer(&l,0)==-7);
    CHECK(!x4_decoder_depth_output_offer(&l,1));CHECK(x4_decoder_depth_outputs_held(&l)==2);
    CHECK(!x4_decoder_depth_output_return(&l,0)&&!l.output_held[0]&&l.output_held[1]);
    x4_decoder_depth_delete_complete(&l);CHECK(l.deleted&&!x4_decoder_depth_inputs_live(&l)&&!x4_decoder_depth_outputs_held(&l));
    CHECK(!x4_decoder_depth_init(&l,1)&&l.input_count==2&&l.output_count==4&&!l.deleted);
    puts("PASS ledger exact: four inputs; input0 reusable only after three subsequent Decode returns; Delete resets every lease");
}
static void continuous_delayed_stream(void)
{
    for(unsigned delay=1;delay<=2;++delay){
        reset_depth();native_delay=delay;X4LiveVideo v={0};depth_start(&v,2,true);
        X4VideoPipelineRing ring;CHECK(!x4_video_pipeline_ring_init_outputs(&ring,delay,5));
        unsigned no_output=0,outputs=0;
        for(unsigned n=0;n<128;++n){
            X4VideoPipelineLease offered,completed;CHECK(!x4_video_pipeline_acquire_decode(&ring,&offered,NULL));
            uint8_t au[sizeof(depth_au)];memcpy(au,depth_au,sizeof(au));au[5]=(uint8_t)(0x80|(n&63));
            X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(&v,offered.slot,au,sizeof(au),n,false,&out));
            CHECK(!out.accepted && out.frame_class==X4_STREAM_FRAME_OK&&!out.ownership_uncertain);
            CHECK(out.offered_retained && x4_live_video_stream_output_held_mask(&v)&(1u<<offered.slot));
            CHECK(out.input_slot==n%4 && out.native_call==n+1);
            if(!out.valid){++no_output;CHECK(!out.completed_output);}
            else{
                ++outputs;CHECK(out.completed_output&&out.slot!=offered.slot&&out.layout_verified);
                CHECK(!(x4_live_video_stream_output_held_mask(&v)&(1u<<out.slot)));
                CHECK(!x4_video_pipeline_decoding_lease(&ring,out.slot,&completed));
                CHECK(!x4_video_pipeline_publish_ready(&ring,completed,out.output_id,out.decode_end_us));
                CHECK(!x4_video_pipeline_take_ready(&ring,&completed,NULL));
                CHECK(!x4_video_pipeline_release_conversion(&ring,completed));
            }
            CHECK(x4_decoder_depth_outputs_held(&((VideoState *)v.state)->stream_depth)==(n<delay?n+1:delay));
        }
        CHECK(no_output==delay&&outputs==128-delay&&v.no_picture_calls==delay);
        CHECK(v.decoded_frames==outputs&&v.decode_calls==128&&!v.error);
        depth_close(&v);CHECK(delete_calls==1);
        printf("PASS real depth2 raw adapter: 128 calls, delay=%u, exact-pointer publication, isAccepted=false, four inputs unreused early, Delete-only close\n",delay);
    }
}
static void idr_wait_bad_frame_and_recreate(void)
{
    reset_depth();X4LiveVideo v={0};depth_start(&v,2,true);damaged_call=2;
    X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(&v,0,depth_au,sizeof(depth_au),1,false,&out)&&!out.valid);
    CHECK(!x4_live_video_stream_raw_decode(&v,1,depth_au,sizeof(depth_au),2,false,&out));
    CHECK(out.frame_class==X4_STREAM_FRAME_BAD_FRAME && out.completed_output && out.slot==0 && !v.error);
    CHECK(x4_live_video_stream_output_held_mask(&v)==2);
    /* The pipeline discards non-IDR while waiting; it never calls Decode for
     * those AU. Native pending output1 is retained over the whole wait. */
    for(unsigned n=0;n<120;++n)CHECK(x4_live_video_stream_output_held_mask(&v)==2&&decode_calls==2);
    CHECK(!x4_live_video_stream_raw_decode(&v,2,depth_au,sizeof(depth_au),123,false,&out));
    CHECK(out.valid&&out.slot==1&&out.frame_class==X4_STREAM_FRAME_OK);
    CHECK(x4_live_video_stream_output_held_mask(&v)==4);
    /* Sending the next IDR may return the older image pending in native.
     * No input/output correlation or automatic BAD_FRAME error is invented. */
    uint8_t sps720[512];size_t n=depth_sps(sps720,false);
    CHECK(!x4_live_video_stream_raw_decode(&v,3,sps720,n,124,false,&out));
    CHECK(out.frame_class==X4_STREAM_FRAME_RECONFIGURE&&!out.completed_output);
    CHECK(x4_live_video_stream_output_held_mask(&v)==4&&decode_calls==3);
    depth_close(&v);
    reset_depth();depth_start(&v,1,false);CHECK(!((VideoState *)v.state)->stream_depth.decode_completed);
    depth_close(&v);reset_depth();depth_start(&v,2,true);depth_close(&v);
    reset_depth();depth_start(&v,1,false);depth_close(&v);
    puts("PASS delayed BAD_FRAME / wait-IDR / old pending output / SPS720->1080->720 recreation with a clean ledger");
}
static int reject_depth_two_query(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)call;return c->decodePipelineDepth==2?-220:0;}
static int reject_depth_two_create(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{(void)call;return c->decodePipelineDepth==2?-221:0;}
static int reject_two_create_with_cleanup_failure(unsigned call,const OrbisVideodec2DecoderConfigInfo *c)
{
    (void)call;if(c->decodePipelineDepth==2){release_rc=-222;return -221;}return 0;
}
static void real_query_memory_arithmetic(void)
{
    /* Values observed in approved console probe 56160be9c111, not supplied
     * by the HOST decoder double and not remeasured here. */
    X4VideoMemoryBlock blocks[13];unsigned n=0;
    for(unsigned i=0;i<4;++i)blocks[n++]=(X4VideoMemoryBlock){2097152,16384};
    blocks[n++]=(X4VideoMemoryBlock){4805120,16384};
    blocks[n++]=(X4VideoMemoryBlock){33548416,16384};
    blocks[n++]=(X4VideoMemoryBlock){34471424,16384};
    blocks[n++]=(X4VideoMemoryBlock){29065728,16384};
    for(unsigned i=0;i<5;++i)blocks[n++]=(X4VideoMemoryBlock){3346432,16384};
    size_t total=0;CHECK(!x4_video_native_memory_total_with_budget(blocks,n,160u*1024u*1024u,&total));
    CHECK(total==127107072 && total+32u*1024u*1024u==160661504);
    CHECK(total+32u*1024u*1024u<192u*1024u*1024u);
    /* Reject a request whose combined native+auxiliary sum crosses192MiB. */
    blocks[5].bytes+=64u*1024u*1024u;
    CHECK(x4_video_native_memory_total_with_budget(blocks,n,160u*1024u*1024u,&total)==X4_VIDEO_CAPACITY_MEMORY);
    puts("PASS real Query arithmetic: 127107072 native + 25165824 VideoOut + 8388608 queued AU = 160661504 < 201326592; over192MiB rejected");
}
static int reject_two_query_cancel(unsigned call,const OrbisVideodec2DecoderConfigInfo *cfg)
{
    (void)call;if(cfg->decodePipelineDepth==2){atomic_store(&cancel_depth,true);return -220;}return 0;
}
static int reject_two_create_cancel(unsigned call,const OrbisVideodec2DecoderConfigInfo *cfg)
{
    (void)call;if(cfg->decodePipelineDepth==2){atomic_store(&cancel_depth,true);return -221;}return 0;
}
static void cancelled_depth_fallback_is_not_executed(void)
{
    uint8_t sps[512];size_t n=depth_sps(sps,true);
    for(unsigned create=0;create<2;++create){
        reset_depth();X4LiveVideo v={0};
        if(create)stream_create_fault=reject_two_create_cancel;else stream_query_fault=reject_two_query_cancel;
        CHECK(x4_live_video_stream_raw_start_cpu_depth(&v,sps,n,&cancel_depth,X4_CPU_0,2)==X4_STREAM_VIDEO_CANCELLED);
        CHECK(query_calls==1&&create_calls==(create?1u:0u));
        CHECK(v.stream_info.attempt_count==1&&v.stream_info.active_attempt==0);
        CHECK(v.stream_info.attempts[0].depth==2&&v.stream_info.attempts[0].cleanup==0);
        CHECK(!v.stream_info.depth_fallback_used&&!v.stream_info.fallback_used&&!v.state&&!reserved_count());
    }
    puts("PASS Query/Create depth2 rejection with concurrent cancellation: no depth1 call, no false executed fallback flag, safe cleanup");
}
static X4LiveVideo *create_error_handle_video;
static int reject_two_create_with_handle_delete_failure(unsigned call,const OrbisVideodec2DecoderConfigInfo *cfg)
{
    (void)call;
    if(cfg->decodePipelineDepth==2){
        /* Test double of Create writing its output handle and still returning
         * an error. The real adapter passed exactly &state->decoder to it. */
        CHECK(create_error_handle_video&&create_error_handle_video->state);
        ((VideoState *)create_error_handle_video->state)->decoder=(void *)(uintptr_t)0x22;
        delete_rc=-601;return -221;
    }
    return 0;
}
static void create_error_handle_failed_delete_forbids_fallback(void)
{
    reset_depth();X4LiveVideo v={0};uint8_t sps[512];size_t n=depth_sps(sps,true);
    create_error_handle_video=&v;stream_create_fault=reject_two_create_with_handle_delete_failure;
    CHECK(x4_live_video_stream_raw_start_cpu_depth(&v,sps,n,&cancel_depth,X4_CPU_0,2)==-221);
    CHECK(query_calls==1&&create_calls==1&&delete_calls==1&&!decode_calls&&!release_calls);
    CHECK(v.state&&((VideoState *)v.state)->decoder==(void *)(uintptr_t)0x22&&reserved_count()==13);
    CHECK(v.stream_info.retained&&v.stream_info.attempt_count==1&&v.stream_info.attempts[0].cleanup==-601);
    CHECK(v.stream_info.delete_decoder_result==-601&&!v.stream_info.depth_fallback_used);
    CHECK(x4_live_video_stream_raw_start_cpu_depth(&v,sps,n,&cancel_depth,X4_CPU_0,1)==-7);
    CHECK(query_calls==1&&create_calls==1&&delete_calls==1&&!release_calls&&reserved_count()==13);
    /* Fixture-only repair proves no leaked HOST memory. Production retains
     * resources and never retries this failed teardown/replacement. */
    delete_rc=0;depth_close(&v);create_error_handle_video=NULL;
    puts("PASS combined Create error with returned handle + failed Delete: all13 blocks retained, zero release, no depth1 fallback/re-create, exact returns preserved");
}
static void rejection_fallback_and_budget(void)
{
    uint8_t sps[512];size_t n=depth_sps(sps,true);
    for(unsigned create=0;create<2;++create){
        reset_depth();X4LiveVideo v={0};
        if(create)stream_create_fault=reject_depth_two_create;else stream_query_fault=reject_depth_two_query;
        CHECK(!x4_live_video_stream_raw_start_cpu_depth(&v,sps,n,&cancel_depth,X4_CPU_0,2));
        CHECK(query_calls==2&&create_calls==(create?2u:1u));
        CHECK(v.stream_info.requested_depth==2&&v.stream_info.active_depth==1&&v.stream_info.depth_fallback_used);
        CHECK(v.stream_info.attempt_count==2&&v.stream_info.attempts[0].depth==2&&v.stream_info.attempts[1].depth==1);
        CHECK(v.stream_info.attempts[0].cleanup==0&&v.stream_info.attempts[0].kind==v.stream_info.attempts[1].kind);
        CHECK(stream_query_configs[0].decodePipelineDepth==2&&stream_query_configs[1].decodePipelineDepth==1);
        CHECK(v.stream_info.output_count==4&&v.stream_info.input_count==2);
        depth_close(&v);
    }
    reset_depth();X4LiveVideo v={0};stream_create_fault=reject_two_create_with_cleanup_failure;
    CHECK(x4_live_video_stream_raw_start_cpu_depth(&v,sps,n,&cancel_depth,X4_CPU_0,2)==-221);
    CHECK(query_calls==1&&create_calls==1&&v.state&&v.stream_info.retained);
    CHECK(v.stream_info.attempts[0].cleanup==-222&&!v.stream_info.depth_fallback_used);
    release_rc=0;depth_close(&v);
    reset_depth();v=(X4LiveVideo){0};depth_start(&v,2,true);
    X4StreamVideoInfo i={0};x4_live_video_stream_info(&v,&i);
    CHECK(i.native_budget_bytes==192u*1024u*1024u&&i.budget_auxiliary_bytes==32u*1024u*1024u);
    CHECK(i.planned_native_bytes+i.budget_auxiliary_bytes<=i.native_budget_bytes);
    CHECK(reserved_count()==13);depth_close(&v);
    reset_depth();v=(X4LiveVideo){0};depth_start(&v,1,true);x4_live_video_stream_info(&v,&i);
    CHECK(i.native_budget_bytes==128u*1024u*1024u&&!i.budget_auxiliary_bytes&&reserved_count()==10);depth_close(&v);
    puts("PASS depth2 Query/Create rejection falls back once to depth1 only after safe cleanup; budget includes native inputs/outputs + 24MiB VideoOut + 8MiB AU queue");
}
static void cancellation_and_failed_delete(void)
{
    reset_depth();X4LiveVideo v={0};depth_start(&v,2,true);native_delay=2;
    X4ProbeNativeOutput out={0};
    CHECK(!x4_live_video_stream_raw_decode(&v,0,depth_au,sizeof(depth_au),1,false,&out));
    CHECK(!x4_live_video_stream_raw_decode(&v,1,depth_au,sizeof(depth_au),2,false,&out));
    CHECK(x4_live_video_stream_output_held_mask(&v)==3);
    atomic_store(&cancel_depth,true);unsigned before=decode_calls;
    CHECK(x4_live_video_stream_raw_decode(&v,2,depth_au,sizeof(depth_au),3,false,&out)==X4_STREAM_VIDEO_CANCELLED);
    CHECK(decode_calls==before&&x4_live_video_stream_output_held_mask(&v)==3);
    delete_rc=-600;CHECK(x4_live_video_stop(&v)==-600&&v.state&&reserved_count()==13&&!release_calls);
    CHECK(x4_live_video_stream_output_held_mask(&v)==3);
    CHECK(x4_decoder_depth_inputs_live(&((VideoState *)v.state)->stream_depth)==2);
    delete_rc=0;depth_close(&v);
    reset_depth();v=(X4LiveVideo){0};depth_start(&v,2,true);native_fail=true;
    CHECK(!x4_live_video_stream_raw_decode(&v,0,depth_au,sizeof(depth_au),1,false,&out));
    CHECK(out.frame_class==X4_STREAM_FRAME_RECOVERABLE_RESTART&&out.ownership_uncertain&&!out.accepted&&out.offered_retained);
    CHECK(x4_live_video_stream_output_held_mask(&v)==1);depth_close(&v);
    reset_depth();v=(X4LiveVideo){0};depth_start(&v,2,true);
    CHECK(!x4_decoder_depth_inputs_live(&((VideoState *)v.state)->stream_depth));depth_close(&v);
    puts("PASS cancellation with two native images; failed Delete retains all 13 blocks and ledger; native error retains before isAccepted; fresh restart ledger");
}
int main(void)
{
    ledger_retirement_exact();continuous_delayed_stream();idr_wait_bad_frame_and_recreate();
    real_query_memory_arithmetic();cancelled_depth_fallback_is_not_executed();create_error_handle_failed_delete_forbids_fallback();rejection_fallback_and_budget();cancellation_and_failed_delete();reset_depth();
    puts("PASS stream depth2 native/ledger HOST MOCK; no PS4 performance or firmware lifetime claim");return 0;
}
