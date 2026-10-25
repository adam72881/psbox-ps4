/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the retained deterministic native-call fixture without changing it.
 * Its reference matcher is synthetic: these tests establish call/ownership
 * behavior, never decoder compatibility or real color/coherence. */
#define main x4_retained_native_fixture_main
#include "probe_native_mock_test.c"
#undef main

static unsigned raw_calls, raw_mode;
static void *raw_buffers[8];
static size_t raw_sizes[8];
enum { RAW_DELAY, RAW_RETURN_CURRENT_UNACCEPTED, RAW_FOREIGN, RAW_ERROR };
static int32_t raw_decode_mock(void *decoder, const OrbisVideodec2InputData *input,
    OrbisVideodec2FrameBuffer *frame, OrbisVideodec2OutputInfo *output)
{
    (void)decoder;
    CHECK(input && input->pAuData && input->auSize);
    unsigned call = raw_calls++;
    CHECK(call < 8);
    raw_buffers[call] = frame->pFrameBuffer; raw_sizes[call] = frame->frameBufferSize;
    frame->isAccepted = raw_mode != RAW_RETURN_CURRENT_UNACCEPTED;
    if (raw_mode == RAW_ERROR) return -930;
    if (raw_mode == RAW_DELAY && call < 2) return 0;
    unsigned from = raw_mode == RAW_DELAY ? call-2 : call;
    OrbisVideodec2FrameBuffer returned = {.pFrameBuffer=raw_buffers[from],.frameBufferSize=raw_sizes[from]};
    output_picture(&returned,output);
    if (raw_mode == RAW_FOREIGN) output->pFrameBuffer = (void *)(uintptr_t)0x10;
    return 0;
}
static int32_t raw_flush_mock(void *decoder, OrbisVideodec2FrameBuffer *frame,
    OrbisVideodec2OutputInfo *output)
{
    (void)decoder; (void)frame; (void)output; ++flush_calls;
    return flush_rc;
}
static int raw_start(X4LiveVideo *v, uint32_t *pixels, atomic_bool *cancel, unsigned case_index)
{
    reset_fixture(); raw_calls=0; raw_mode=RAW_DELAY;
    memset(raw_buffers,0,sizeof(raw_buffers)); memset(raw_sizes,0,sizeof(raw_sizes));
    int rc=x4_live_video_probe_raw_start(v,pixels,X4_LIVE_RGB_PIXELS,
        &x4_probe_cases[case_index],&hash_context,cancel);
    if (rc) return rc;
    VideoState *state=v->state;
    CHECK(state->raw_pipeline && state->probe);
    CHECK(!state->staging && !state->staging_allocation && !state->parallel_enabled);
    CHECK(v->probe_status.staging_bytes==0);
    for (unsigned i=0;i<COPY_HELPERS;++i) CHECK(!state->helpers[i].running);
    for (unsigned i=0;i<RING;++i) CHECK(reservations[state->output[i].offset].type==0);
    state->Decode=raw_decode_mock; state->Flush=raw_flush_mock; valid_output=true;
    return 0;
}
static void delayed_outputs(uint32_t *pixels)
{
    atomic_bool cancel=false; X4LiveVideo v={0};
    CHECK(raw_start(&v,pixels,&cancel,0)==0);
    const uint8_t bytes[]={0x65,0x80}; X4ProbeNativeOutput out;
    CHECK(x4_live_video_probe_raw_decode(&v,0,bytes,sizeof(bytes),10,false,&out)==0);
    CHECK(out.accepted && !out.valid && out.slot==RING && raw_calls==1);
    CHECK(x4_live_video_probe_raw_decode(&v,1,bytes,sizeof(bytes),11,false,&out)==0);
    CHECK(out.accepted && !out.valid && out.slot==RING);
    CHECK(x4_live_video_probe_raw_decode(&v,2,bytes,sizeof(bytes),12,false,&out)==0);
    CHECK(out.accepted && out.valid && out.slot==0 && !out.layout_verified);
    CHECK(out.pixels==raw_buffers[0] && out.pixels!=raw_buffers[2] && out.decode_us==1);
    CHECK(v.probe_status.layout_reference_matches==1 && out.output_id==1);
    CHECK(x4_live_video_probe_raw_decode(&v,3,bytes,sizeof(bytes),13,false,&out)==0);
    CHECK(out.accepted && out.valid && out.slot==1 && !out.layout_verified);
    /* Slot zero was returned in the third call. The caller may now offer it
     * again after finishing its reference read; slots two/three stay native-held. */
    CHECK(x4_live_video_probe_raw_decode(&v,0,bytes,sizeof(bytes),14,false,&out)==0);
    CHECK(out.accepted && out.valid && out.slot==2 && out.layout_verified);
    CHECK(v.probe_status.layout_reference_matches==3 && v.probe_status.layout_checks==3);
    CHECK(out.layout.visible_width==1280 && out.layout.visible_height==720);
    CHECK(out.layout.storage_height==720 && out.layout.uv_plane_offset==921600);
    CHECK(out.decode_us==1 && v.decode_calls==5 && v.decoded_frames==3);
    CHECK(v.copy_calls==0 && v.frames==0 && v.probe_status.staging_bytes==0);
    CHECK(x4_live_video_probe_raw_decode(&v,1,bytes,sizeof(bytes),15,false,&out)==0);
    CHECK(out.valid && out.slot==3 && out.layout_verified && v.probe_status.layout_checks==3);
    CHECK(x4_live_video_stop(&v)==0 && !v.state && reserved_count()==0 && join_calls==0);
    puts("PASS raw adapter: delayed output identifies returned slot, three reference checks, no staging/copy/helpers");
}
static void raw_failure_paths(uint32_t *pixels)
{
    atomic_bool cancel=false; X4LiveVideo v={0}; const uint8_t bytes[]={0x65,0x80};
    X4ProbeNativeOutput out;
    CHECK(raw_start(&v,pixels,&cancel,0)==0); raw_mode=RAW_RETURN_CURRENT_UNACCEPTED;
    CHECK(x4_live_video_probe_raw_decode(&v,0,bytes,sizeof(bytes),1,false,&out)==0);
    CHECK(!out.accepted && out.valid && out.slot==0);
    CHECK(x4_live_video_stop(&v)==0 && !reserved_count());

    v=(X4LiveVideo){0}; CHECK(raw_start(&v,pixels,&cancel,0)==0);
    raw_mode=RAW_FOREIGN;
    CHECK(x4_live_video_probe_raw_decode(&v,0,bytes,sizeof(bytes),1,false,&out)<0);
    CHECK(!out.valid && v.probe_status.output_reject_reason==4);
    CHECK(x4_live_video_stop(&v)==0 && !reserved_count());

    v=(X4LiveVideo){0}; CHECK(raw_start(&v,pixels,&cancel,0)==0); raw_mode=RAW_ERROR;
    CHECK(x4_live_video_probe_raw_decode(&v,0,bytes,sizeof(bytes),1,false,&out)==-930);
    CHECK(out.accepted && !out.valid && out.native_result==-930 && v.error==-930);
    CHECK(v.probe_status.decode_result==-930 && raw_calls==1);
    CHECK(x4_live_video_probe_raw_decode(&v,1,bytes,sizeof(bytes),2,false,&out)<0 && raw_calls==1);
    unsigned held=reserved_count(); delete_rc=-931;
    CHECK(x4_live_video_stop(&v)==-931 && v.state && reserved_count()==held && !release_calls);
    /* Fixture-only cleanup after injected failure. Production must latch the
     * unsafe close and keep all references; it must not retry this deletion. */
    delete_rc=0; CHECK(x4_live_video_stop(&v)==0 && !reserved_count());

    v=(X4LiveVideo){0}; CHECK(raw_start(&v,pixels,&cancel,0)==0);
    CHECK(x4_live_video_probe_raw_decode(&v,0,NULL,0,0,true,&out)==0);
    CHECK(!out.valid && !out.accepted && flush_calls==1);
    flush_rc=-932;
    CHECK(x4_live_video_probe_raw_decode(&v,1,NULL,0,0,true,&out)==-932);
    CHECK(v.probe_status.flush_result==-932 && !out.valid);
    CHECK(x4_live_video_stop(&v)==0 && !reserved_count());

    v=(X4LiveVideo){0}; CHECK(raw_start(&v,pixels,&cancel,0)==0);
    atomic_store(&cancel,true);
    CHECK(x4_live_video_probe_raw_decode(&v,0,bytes,sizeof(bytes),1,false,&out)==X4_PROBE_CANCELLED);
    CHECK(x4_live_video_probe_raw_decode(&v,0,NULL,0,0,true,&out)==X4_PROBE_CANCELLED);
    CHECK(raw_calls==0 && flush_calls==0);
    CHECK(x4_live_video_stop(&v)==0 && !reserved_count());
    atomic_store(&cancel,false);
    puts("PASS raw adapter: accepted/output independent, foreign pointer rejected, delete-before-release, errors/cancel gate");
}
static void raw_layout_change(uint32_t *pixels)
{
    atomic_bool cancel=false; X4LiveVideo v={0}; const uint8_t bytes[]={0x65,0x80};
    X4ProbeNativeOutput out;
    CHECK(raw_start(&v,pixels,&cancel,0)==0); raw_mode=RAW_RETURN_CURRENT_UNACCEPTED;
    for(unsigned i=0;i<3;++i)
        CHECK(x4_live_video_probe_raw_decode(&v,i,bytes,sizeof(bytes),i,false,&out)==0);
    CHECK(out.layout_verified);
    output_height_override=718;
    CHECK(x4_live_video_probe_raw_decode(&v,3,bytes,sizeof(bytes),3,false,&out)<0 && !out.valid);
    CHECK(v.probe_status.layout_verdict==X4_PROBE_LAYOUT_INDETERMINATE ||
        v.probe_status.layout_verdict==X4_PROBE_LAYOUT_FAILED);
    CHECK(v.copy_calls==0 && x4_live_video_stop(&v)==0 && !reserved_count());
#if X4_VIDEO_CAPACITY_1080
    v=(X4LiveVideo){0}; CHECK(raw_start(&v,pixels,&cancel,2)==0); raw_mode=RAW_RETURN_CURRENT_UNACCEPTED;
    for(unsigned i=0;i<3;++i)
        CHECK(x4_live_video_probe_raw_decode(&v,i,bytes,sizeof(bytes),i,false,&out)==0);
    CHECK(out.layout_verified && out.native_width==1920 && out.native_height==1088);
    CHECK(out.layout.visible_width==1920 && out.layout.visible_height==1080);
    CHECK(out.pitch==1920 && out.layout.uv_plane_offset==2088960);
    CHECK(v.copy_calls==0 && x4_live_video_stop(&v)==0 && !reserved_count());
#else
    v=(X4LiveVideo){0}; CHECK(raw_start(&v,pixels,&cancel,2)<0);
    CHECK(!query_calls && !create_calls && !v.state && !reserved_count());
#endif
    puts("PASS raw adapter: geometry change fails closed, 1080 remains OFF-gated and ON-bounded");
}
static void raw_joint_budget(uint32_t *pixels)
{
#if X4_VIDEO_CAPACITY_1080
    atomic_bool cancel=false;X4LiveVideo v={0};reset_fixture();
    /* The decoder plan alone fits 128 MiB; with the three 8 MiB scanouts it
     * must reject BEFORE allocating native decoder memory or creating it. */
    query_cpu_bytes=query_gpu_bytes=52u*1024u*1024u;
    int rc=x4_live_video_probe_raw_start(&v,pixels,X4_LIVE_RGB_PIXELS,
        &x4_probe_cases[0],&hash_context,&cancel);
    CHECK(rc<0 && query_calls==1 && !create_calls && !v.state && !reserved_count());
    CHECK(next_reservation==3); /* Two AU inputs and compute queue were required for Query. */
    puts("PASS raw adapter: joint budget rejects before decoder/output allocations and releases query prerequisites");
#else
    (void)pixels;
#endif
}
int main(void)
{
    uint32_t *pixels=calloc(X4_LIVE_RGB_PIXELS,sizeof(*pixels)); CHECK(pixels);
    delayed_outputs(pixels); raw_failure_paths(pixels); raw_layout_change(pixels);raw_joint_budget(pixels);
    reset_fixture(); free(pixels);
    printf("PASS raw native adapter HOST MOCK capacity=%u checks=%u; no hardware/color/coherence claim\n",
        (unsigned)X4_VIDEO_CAPACITY_1080,checks);
    return 0;
}
