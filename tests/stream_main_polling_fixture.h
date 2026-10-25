/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by the actual scheduler fixture; no worker or native timing claim. */
static void polling_flip(X4StreamPipeline *p,uint64_t submitted,uint64_t ended,uint64_t observed)
{
    atomic_store(&fixture_clock,submitted);flip_time_override=ended;
    X4VideoPipelineLease lease;
    assert(!x4_video_pipeline_acquire_scanout(&p->display,&lease));
    assert(!x4_video_pipeline_finish_scanout(&p->display,lease,submitted,submitted-200,submitted-100));
    assert(!submit_display(p));pending_polls=0;
    assert(x4_stream_pipeline_main_state(p)==X4_STREAM_MAIN_FLIP_PENDING);
    atomic_store(&fixture_clock,observed);assert(!observe_display(p));
    /* SCREEN stays owned by VideoOut, but is no longer in flight. */
    assert(!x4_stream_pipeline_main_state(p));
}
static void main_polling_measurements(void)
{
    reset();atomic_store(&fixture_clock,100);atomic_bool stop;X4StreamPipeline *p=new_pipeline(&stop,140);
    assert(!x4_stream_pipeline_main_state(NULL));
    /* Hold both existing gates: the same-owner state query must not lock. */
    lock(&p->ring_gate);lock(&p->status_gate);
    uint64_t calls=x4_stream_wait_test_api_calls();
    assert(!x4_stream_pipeline_main_state(p));p->converting=true;
    assert(x4_stream_pipeline_main_state(p)==X4_STREAM_MAIN_CONVERTING);
    p->display.pending=0;
    assert(x4_stream_pipeline_main_state(p)==(X4_STREAM_MAIN_CONVERTING|X4_STREAM_MAIN_FLIP_PENDING));
    p->converting=false;
    assert(x4_stream_pipeline_main_state(p)==X4_STREAM_MAIN_FLIP_PENDING);
    p->display.pending=-1;
    assert(x4_stream_wait_test_api_calls()==calls);
    unlock(&p->status_gate);unlock(&p->ring_gate);
    x4_stream_pipeline_note_controller_read(p,1000);
    x4_stream_pipeline_note_controller_read(p,2000);
    x4_stream_pipeline_note_controller_read(p,5000);
    assert(!x4_stream_pipeline_display_open(p));
    polling_flip(p,1000,1100,1500);assert(p->flip_time_valid==1);
    note_conversion_begin(p,1600);assert(!p->flip_to_conversion_pending);
    polling_flip(p,2000,2100,2500);note_conversion_begin(p,2800);
    /* Before-submit, future and absent native timestamps must be omitted. */
    polling_flip(p,3000,2999,3500);assert(!p->flip_to_conversion_pending);
    polling_flip(p,4000,4501,4500);assert(!p->flip_to_conversion_pending);
    polling_flip(p,5000,0,5500); /* fixture 0 means native completion now */
    assert(p->flip_time_valid==3);p->flip_to_conversion_pending=false;
    /* Explicitly absent ptime is exercised through the real observer. */
    atomic_store(&fixture_clock,6000);X4VideoPipelineLease lease;
    assert(!x4_video_pipeline_acquire_scanout(&p->display,&lease));
    assert(!x4_video_pipeline_finish_scanout(&p->display,lease,6000,5800,5900));
    assert(!submit_display(p));pending_polls=0;
    /* Consume the host completion once, then present the same pending arg
     * with an absent timestamp to the production observer. */
    OrbisVideoOutFlipStatus status={0};assert(!sceVideoOutGetFlipStatus(7,&status));shown_time=0;
    atomic_store(&fixture_clock,6500);assert(!observe_display(p));
    assert(p->flip_time_invalid==3&&!p->flip_to_conversion_pending);
    char path[512];int n=snprintf(path,sizeof(path),"%s/main-polling-measurements.json",report_directory);
    assert(n>0&&(size_t)n<sizeof(path));FILE *f=fopen(path,"wb");assert(f);
    assert(!x4_stream_diagnostics_write(f,p->diagnostics,7000));assert(!fclose(f));
    atomic_store(&fixture_clock,0);assert(!x4_stream_pipeline_close(p));
    assert(!x4_stream_pipeline_main_state(p));assert(!x4_stream_pipeline_free(p));
    puts("PASS main-owned state without new lock, pad interval mean/max, native flip end distinct from observation and invalid timestamps omitted");
}
