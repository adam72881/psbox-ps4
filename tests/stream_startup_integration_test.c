/* SPDX-License-Identifier: GPL-3.0-only */
/* Real SPS parser/raw adapter/scheduler. Reuse only the existing host native,
 * VideoOut, bounded-stack and controlled-clock fixtures. No PS4 timing claim. */
#define X4_STREAM_RESILIENCE_NO_MAIN 1
#include "stream_resilience_integration_test.c"

static X4StreamPipeline *begin_startup(unsigned session,unsigned profile,bool publish_alias)
{
    reset_host();atomic_store(&controlled_clock_us,1000000);
    X4StreamRequestConfig request=x4_stream_request_config((X4StreamRequestProfile)profile,X4_COLOR_AUTO_709);
    request.decoder_depth=X4_DEPTH_1; /* Preserve the startup-only single-call fixture contract. */
    current=x4_stream_pipeline_create_request(session,NULL,&external_stop,&request);assert(current);
    assert(x4_stream_pipeline_startup(current)==&current->startup);
    if(publish_alias)x4_stream_startup_note_alias(&current->startup,0,sceKernelGetProcessTime());
    assert(!x4_stream_pipeline_start(current));assert(!x4_stream_pipeline_display_open(current));return current;
}
static void startup_switch(unsigned profile,unsigned session)
{
    X4StreamPipeline *p=begin_startup(session,profile,true);
    Fixture large=dimensions(2560,1440);large.level=51;
    for(unsigned tag=1;tag<=10;++tag){
        assert(!submit_frame(p,&large,tag,true));pump_dequeued(p,tag);
        assert(!queries&&!creates&&!native_decodes&&!snapshot(p).error);
        assert(snapshot(p).rejected_width==2560&&snapshot(p).rejected_height==1440);
        assert(x4_stream_startup_waiting(&p->startup));
        (void)x4_stream_pipeline_take_keyframe_request(p);
        advance_clock_and_wait_for_idle_poll(p,300000);
        assert(consume_keyframe_after_publication(p));
    }
    Fixture admitted=profile==X4_REQUEST_720P_HQ?dimensions(1280,720):dimensions(1920,1088);
    assert(!submit_frame(p,&admitted,11,true));pump_outputs(p,1);
    assert(p->startup.admitted&&p->startup.elapsed_available&&p->startup.alias_to_sps_us==3000000);
    assert(p->startup.dropped_aus==10&&!x4_stream_startup_waiting(&p->startup));
    assert(!snapshot(p).rejected_width&&!snapshot(p).rejected_height);
    assert(queries==1&&creates==1&&native_decodes==1&&query_configs[0].maxFrameWidth==admitted.width);
    assert(!atomic_load(&external_stop)&&!snapshot(p).frame_errors);
    finish_case(p);
    puts("PASS real startup: three seconds of 1440p never reach Query/Create/Decode; first supported SPS starts and presents with exact alias-return-to-SPS time");
}
static void startup_timeout(void)
{
    X4StreamPipeline *p=begin_startup(202,X4_REQUEST_1080P_G,true);
    Fixture large=dimensions(2560,1440);large.level=51;
    assert(!submit_frame(p,&large,1,true));pump_dequeued(p,1);
    advance_clock_and_wait_for_idle_poll(p,9999999);assert(!snapshot(p).error);
    advance_clock_and_wait_for_idle_poll(p,1);
    assert(snapshot(p).error==X4_STREAM_INITIAL_RESOLUTION_TIMEOUT&&!queries&&!creates&&!native_decodes);
    assert(snapshot(p).rejected_width==2560&&snapshot(p).rejected_height==1440);
    assert(!atomic_load(&external_stop)&&!x4_stream_startup_waiting(&p->startup));finish_case(p);
    p=begin_startup(203,X4_REQUEST_1080P_G,true);
    advance_clock_and_wait_for_idle_poll(p,10000000);
    assert(snapshot(p).error==X4_STREAM_INITIAL_RESOLUTION_TIMEOUT&&!queries&&!creates&&!native_decodes);
    assert(!atomic_load(&external_stop));finish_case(p);
    puts("PASS real startup: exact ten-second timeout after local alias success, including no further AU, keeps audio/RTC cancellation false and retains observed dimensions");
}
static void startup_before_alias(void)
{
    X4StreamPipeline *p=begin_startup(204,X4_REQUEST_1080P_G,false);
    Fixture large=dimensions(2560,1440);large.level=51;
    assert(!submit_frame(p,&large,1,true));pump_dequeued(p,1);
    advance_clock_and_wait_for_idle_poll(p,60000000);
    assert(!snapshot(p).error&&!queries&&!creates&&!p->startup.timed_out);
    assert(!x4_stream_pipeline_take_keyframe_request(p));
    x4_stream_startup_note_alias(&p->startup,0,sceKernelGetProcessTime());
    advance_clock_and_wait_for_idle_poll(p,3000000);
    Fixture supported=dimensions(1920,1088);assert(!submit_frame(p,&supported,2,true));pump_outputs(p,1);
    assert(p->startup.alias_to_sps_us==3000000&&queries==1);finish_case(p);
    puts("PASS real startup: no deadline or keyframe is invented before the alias send; its later success starts the full ten-second window");
}
static void startup_default720_and_cancel(void)
{
    X4StreamPipeline *p=begin_startup(205,X4_REQUEST_720P60,false);
    Fixture large=dimensions(2560,1440);large.level=51;
    assert(!submit_frame(p,&large,1,true));uint64_t deadline=host_wall_us()+2000000;
    while(!snapshot(p).error){assert(host_wall_us()<deadline);sceKernelUsleep(100);}
    assert(snapshot(p).error==X4_SPS_SIZE&&!queries&&!creates&&!native_decodes&&!p->startup.dropped_aus);
    finish_case(p);
    p=begin_startup(206,X4_REQUEST_1080P_G,true);
    assert(!submit_frame(p,&large,1,true));pump_dequeued(p,1);
    atomic_store(&external_stop,true);x4_stream_pipeline_request_stop(p);finish_case(p);
    assert(!queries&&!creates&&!native_decodes);
    puts("PASS default720 keeps immediate rejection and cancellation during aliased wait creates no decoder or retained native buffers");
}
static void startup_does_not_mask_reconfiguration(void)
{
    X4StreamPipeline *p=begin_startup(208,X4_REQUEST_1080P_G,true);
    Fixture supported=dimensions(1920,1088),large=dimensions(2560,1440);large.level=51;
    assert(!submit_frame(p,&supported,1,true));pump_outputs(p,1);
    assert(!submit_frame(p,&large,2,true));uint64_t deadline=host_wall_us()+2000000;
    while(!snapshot(p).error){(void)x4_stream_pipeline_present(p,NULL,NULL);assert(host_wall_us()<deadline);sceKernelUsleep(100);}
    assert(snapshot(p).error==X4_SPS_SIZE&&queries==1&&creates==1&&native_decodes==1);
    assert(!p->startup.dropped_aus&&!x4_stream_startup_waiting(&p->startup));finish_case(p);
    puts("PASS later oversized SPS remains a reconfiguration failure, never a renewed initial-resolution wait");
}
static void startup_does_not_mask_format_errors(void)
{
    for(unsigned variant=0;variant<3;++variant){
        X4StreamPipeline *p=begin_startup(209+variant,X4_REQUEST_1080P_G,true);
        Fixture bad=dimensions(2560,1440);bad.level=51;
        int expected;
        if(!variant){bad.frame=0;expected=X4_SPS_SIZE;}
        else if(variant==1){bad.profile=100;bad.chroma=2;expected=X4_SPS_CHROMA;}
        else {bad.profile=110;bad.depth_l=bad.depth_c=10;expected=X4_SPS_BIT_DEPTH;}
        assert(!submit_frame(p,&bad,1,true));uint64_t deadline=host_wall_us()+2000000;
        while(!snapshot(p).error){assert(host_wall_us()<deadline);sceKernelUsleep(100);}
        assert(snapshot(p).error==expected&&!queries&&!creates&&!native_decodes&&!p->startup.dropped_aus);
        finish_case(p);
    }
    puts("PASS interlaced, non-420 and >8-bit oversized SPS do not receive the startup exception");
}
static void startup_supported_before_alias_and_malformed(void)
{
    X4StreamPipeline *p=begin_startup(212,X4_REQUEST_1080P_G,false);
    Fixture supported=dimensions(1920,1088);
    assert(!submit_frame(p,&supported,1,true));pump_outputs(p,1);
    assert(p->startup.admitted&&!p->startup.elapsed_available&&!p->startup.alias_to_sps_us);
    assert(!x4_stream_startup_waiting(&p->startup)&&queries==1&&native_decodes==1);
    x4_stream_startup_note_alias(&p->startup,0,sceKernelGetProcessTime());
    advance_clock_and_wait_for_idle_poll(p,20000000);
    assert(!snapshot(p).error&&!x4_stream_startup_waiting(&p->startup));finish_case(p);

    p=begin_startup(213,X4_REQUEST_1080P_G,true);
    const uint8_t truncated[]={0,0,0,1,0x67};
    const uint8_t pps[]={0,0,0,1,0x68,0xce,0x38,0x80};
    const uint8_t au[]={0,0,0,1,0x65,0x80};
    assert(!x4_stream_pipeline_submit_parameters(p,au,sizeof(au),0,1,truncated,sizeof(truncated),pps,sizeof(pps)));
    pump_dequeued(p,1);
    assert(!snapshot(p).error&&p->waiting_idr&&!p->startup.dropped_aus&&!queries&&!creates);
    advance_clock_and_wait_for_idle_poll(p,5000000);
    assert(snapshot(p).error==X4_STREAM_IDR_WAIT_TIMEOUT&&!queries&&!creates&&!native_decodes);
    finish_case(p);
    puts("PASS supported SPS before alias starts without invented timing; malformed SPS keeps its existing five-second IDR recovery, never the oversized ten-second exception");
}
int main(int argc,char **argv)
{
    assert(argc==2);report_directory=argv[1];
    startup_switch(X4_REQUEST_1080P_G,201);startup_timeout();startup_before_alias();
    startup_default720_and_cancel();startup_switch(X4_REQUEST_720P_HQ,207);
    startup_does_not_mask_reconfiguration();startup_does_not_mask_format_errors();
    startup_supported_before_alias_and_malformed();
    puts("PASS real initial-resolution integration: no server or PS4 execution/performance claim");return 0;
}
