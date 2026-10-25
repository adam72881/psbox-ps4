/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual runner, ring, converters and raw native adapter; native decoder,
 * VideoOut, sample reader, report location and metrics sink are host mocks.
 * No fixture result is console compatibility/performance evidence. */
#define X4_NATIVE_MOCK_RUNTIME 1
#define clock_us fixture_clock_us
#define main x4_retained_native_fixture_main
#include "probe_native_mock_test.c"
#undef main
#undef clock_us
#include "../src/video/probe_pipeline.h"
#include "../src/video/video_pipeline.h"
#include "../src/video/probe_files.h"
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>
#include <sys/stat.h>

static const X4ProbeCase *runtime_case;
static X4ProbeCase runtime_descriptor;
static const char *runtime_directory;
static unsigned runtime_frames, runtime_source_at, runtime_cancel_tick;
static int runtime_source_failure, runtime_open_failure, runtime_map_failure, runtime_close_failure;
static bool runtime_metadata_mismatch;
static unsigned runtime_source_opened, runtime_source_closed, runtime_flip_calls, runtime_flip_observations;
static void *runtime_scanout[3];
static int runtime_video_open, runtime_pending=-1, runtime_screen=-1;
static uint64_t runtime_pending_arg, runtime_current_arg;
static uint64_t runtime_previous_case_arg;
static unsigned runtime_stale_previous_polls;
static size_t runtime_allocations;
static void *runtime_context;
static pthread_t runtime_main_thread, runtime_decode_thread;
static unsigned runtime_source_next_calls;
static unsigned runtime_next_failure_at, runtime_decode_failure_at;
static bool runtime_cancel_after_flip;
static bool runtime_incomplete_eof;
static bool runtime_finish_rejected;
static unsigned runtime_flip_delay, runtime_pending_polls;
static uint32_t runtime_pending_pixel, runtime_screen_pixel;
static void runtime_assert_observation_count(void);

/* Tiny atomic metrics sink checks runner lifecycle; the production metrics
 * implementation has its own existing tests. Keep original adapter timing
 * mock, avoiding two implementations of the same native fixture. */
struct X4ProbeMetrics { atomic_uint_fast64_t events; bool ended; };
X4ProbeMetrics *x4_probe_metrics_create(uint64_t time)
{ (void)time; X4ProbeMetrics *m=calloc(1,sizeof(*m)); if(m)atomic_init(&m->events,0); return m; }
void x4_probe_metrics_free(X4ProbeMetrics *m) { free(m); }
bool x4_probe_metrics_begin_measurement(X4ProbeMetrics *m,uint64_t time)
{ (void)time; return m && !m->ended; }
void x4_probe_metrics_note_at(X4ProbeMetrics *m,uint64_t time,X4ProbeCounter c,uint64_t n)
{ (void)time; (void)c; if(m)atomic_fetch_add(&m->events,n); }
void x4_probe_metrics_end(X4ProbeMetrics *m,uint64_t time,unsigned reason)
{ (void)time; (void)reason; if(m)m->ended=true; }
void x4_probe_metrics_record_stack(X4ProbeMetrics *m,unsigned slot,const X4ProbeStackReport *r)
{ (void)m; CHECK(slot<4 && r); }
void x4_probe_metrics_stacks_json(FILE *f,const X4ProbeMetrics *m)
{ (void)m; fputs("[]",f); }
int x4_probe_metrics_dump_file(const X4ProbeMetrics *m,const char *path)
{
    if(!m || !m->ended)return -1;
    FILE *f=fopen(path,"wbx"); if(!f)return -1;
    fprintf(f,"{\"host_mock\":true,\"events\":%llu}\n",(unsigned long long)atomic_load(&m->events));
    return fclose(f);
}
int x4_probe_hash_context_init(X4ProbeHashContext *h)
{ h->buffer=malloc(X4_PROBE_READ_CHUNK);h->capacity=h->buffer?X4_PROBE_READ_CHUNK:0;return h->buffer?0:-10; }
void x4_probe_hash_context_close(X4ProbeHashContext *h)
{ free(h->buffer);h->buffer=NULL;h->capacity=0; }
int x4_probe_source_open_fully_verified(X4ProbeSource *s,const char *clip,const char *index,
    const uint8_t a[32],const uint8_t b[32],X4ProbeHashFile hash,void *ctx,const atomic_bool *cancel)
{
    (void)clip;(void)index;(void)a;(void)b;(void)hash;(void)ctx;(void)cancel;
    CHECK(!pthread_equal(pthread_self(),runtime_main_thread));runtime_decode_thread=pthread_self();
    ++runtime_source_opened; if(runtime_source_failure)return runtime_source_failure;
    s->state=s; const X4ProbeCase *c=runtime_case;
    s->info=(X4ProbeSourceInfo){.records=c->au_count,.duration_us=c->duration_us,
        .visible_width=c->visible_width,.visible_height=c->visible_height,
        .coded_width=c->coded_width,.coded_height=c->coded_height,.fps_num=c->fps_num,.fps_den=c->fps_den};
    if(runtime_metadata_mismatch)++s->info.visible_width;
    return 0;
}
int x4_probe_source_begin(X4ProbeSource *s,uint64_t origin,X4ProbeMode mode)
{ (void)s;(void)origin;(void)mode;CHECK(pthread_equal(pthread_self(),runtime_decode_thread));runtime_source_at=0;return 0; }
int x4_probe_source_next(X4ProbeSource *s,uint64_t time,uint8_t *dst,size_t bytes,X4ProbeAU *out)
{
    (void)s;(void)time;CHECK(bytes>=10 && pthread_equal(pthread_self(),runtime_main_thread));
    ++runtime_source_next_calls;
    if(runtime_source_at==runtime_next_failure_at)return -906;
    if(runtime_source_at==runtime_frames)return X4_PROBE_EOF;
    dst[0]=0x65;dst[1]=0x80;
    uint64_t identity=runtime_source_at;memcpy(dst+2,&identity,sizeof(identity));
    *out=(X4ProbeAU){.ordinal=runtime_source_at,.bytes=10,.pts_us=runtime_source_at*16667u};
    ++runtime_source_at;return 0;
}
int x4_probe_source_close(X4ProbeSource *s)
{ CHECK(pthread_equal(pthread_self(),runtime_decode_thread));if(s->state)++runtime_source_closed;s->state=NULL;return 0; }
/* Runtime fixture uses maximum-speed only. Scheduling arithmetic/paced mode
 * remain covered by the existing real source reader tests. */
int x4_probe_schedule(uint64_t origin,uint64_t pts,uint64_t now,X4ProbeMode mode,uint64_t *wait,uint64_t *late)
{ (void)origin;(void)pts;(void)now;CHECK(mode==X4_PROBE_MAX_SPEED);*wait=*late=0;return 0; }
int x4_probe_files_report_path(char *out,size_t capacity,const char *key,uint64_t sequence)
{
    int n=snprintf(out,capacity,"%s/%llu-%s.json",runtime_directory,(unsigned long long)sequence,key);
    return n<0 || (size_t)n>=capacity ? -1 : 0;
}
FILE *x4_probe_files_report_create(const char *path,X4ProbeFileStatus *status)
{ (void)status;return fopen(path,"wbx"); }
int x4_probe_files_report_finish(FILE *f,X4ProbeFileFinish *finish)
{ (void)finish;return fclose(f); }
void x4_controller_read(X4Controller *pad,unsigned tick)
{ pad->pressed=(runtime_cancel_tick && tick>=runtime_cancel_tick) || (runtime_cancel_after_flip && runtime_flip_calls) ? ORBIS_PAD_BUTTON_CIRCLE : 0; }
int sceVideoOutOpen(int user,int bus,int index,const void *parameters)
{
    (void)user;(void)bus;(void)index;(void)parameters;
    if(runtime_open_failure)return runtime_open_failure;
    CHECK(!runtime_video_open);runtime_video_open=1;return 17;
}
int sceVideoOutClose(int handle)
{
    CHECK(handle==17 && runtime_video_open);
    if(runtime_close_failure)return runtime_close_failure;
    runtime_previous_case_arg=runtime_current_arg;
    runtime_video_open=0;runtime_pending=runtime_screen=-1;return 0;
}
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *a,int format,int tile,int aspect,
    unsigned width,unsigned height,unsigned pitch)
{ (void)format;(void)tile;(void)aspect;*a=(OrbisVideoOutBufferAttribute){width,height,pitch}; }
int sceVideoOutRegisterBuffers(int handle,int first,void *const *buffers,int count,const OrbisVideoOutBufferAttribute *a)
{
    CHECK(handle==17 && first==0 && count==3 && a->width==1920 && a->height==1080 && a->pitch==1920);
    for(unsigned i=0;i<3;++i)runtime_scanout[i]=buffers[i];return runtime_map_failure;
}
int sceVideoOutSetFlipRate(int handle,int rate)
{ (void)rate;CHECK(handle==17);return 0; }
int sceVideoOutSubmitFlip(int handle,int slot,int vsync,int64_t arg)
{
    (void)vsync;CHECK(handle==17 && runtime_video_open && slot>=0 && slot<3);
    CHECK(runtime_pending<0 && slot!=runtime_screen && (uint64_t)arg>runtime_current_arg);
    CHECK(arg>0 && (uint64_t)arg>=(UINT64_C(1)<<62));
    const uint32_t *pixels=runtime_scanout[slot];
    CHECK((pixels[0]>>24)==255 && pixels[0]==pixels[1920u*1080u-1]);
    runtime_pending_pixel=pixels[0];runtime_pending_polls=runtime_flip_delay;
    runtime_pending=slot;runtime_pending_arg=(uint64_t)arg;++runtime_flip_calls;return 0;
}
int sceVideoOutGetFlipStatus(int handle,OrbisVideoOutFlipStatus *s)
{
    CHECK(handle==17 && runtime_video_open);
    runtime_assert_observation_count();
    if(runtime_screen>=0) {
        const uint32_t *pixels=runtime_scanout[runtime_screen];
        CHECK(pixels[0]==runtime_screen_pixel && pixels[1920u*1080u-1]==runtime_screen_pixel);
    }
    if(runtime_pending>=0) {
        const uint32_t *pixels=runtime_scanout[runtime_pending];
        CHECK(pixels[0]==runtime_pending_pixel && pixels[1920u*1080u-1]==runtime_pending_pixel);
        if(runtime_pending_polls) {
            --runtime_pending_polls;
            if(runtime_previous_case_arg && runtime_current_arg==runtime_previous_case_arg)
                ++runtime_stale_previous_polls;
        }
        else {
            runtime_screen=runtime_pending;runtime_pending=-1;runtime_current_arg=runtime_pending_arg;
            runtime_screen_pixel=runtime_pending_pixel;++runtime_flip_observations;
        }
    }
    *s=(OrbisVideoOutFlipStatus){.num=runtime_flip_observations,.flipArg=(int64_t)runtime_current_arg,.ptime=0};return 0;
}
int sceGnmSubmitDone(void) { return 0; }
static int32_t runtime_empty_flush(void *d,OrbisVideodec2FrameBuffer *f,OrbisVideodec2OutputInfo *o)
{ (void)d;(void)f;(void)o;++flush_calls;return 0; }
static int32_t runtime_decode(void *d,const OrbisVideodec2InputData *i,OrbisVideodec2FrameBuffer *f,OrbisVideodec2OutputInfo *o)
{
    CHECK(pthread_equal(pthread_self(),runtime_decode_thread));CHECK(!pthread_equal(pthread_self(),runtime_main_thread));
    uint64_t identity=UINT64_MAX;CHECK(i->auSize==10);memcpy(&identity,(const uint8_t *)i->pAuData+2,sizeof(identity));
    CHECK(identity==decode_calls); /* Bounded input queue must not overwrite an in-use AU. */
    if(decode_calls==runtime_decode_failure_at)return -907;
    int rc=decode(d,i,f,o);
    if(!rc && o->isValid)memset(o->pFrameBuffer,16u+(unsigned)(identity%180),
        (size_t)seen_config.maxFrameWidth*seen_config.maxFrameHeight);
    return rc;
}
static int runtime_raw_start(X4LiveVideo *v,uint32_t *pixels,size_t count,const X4ProbeCase *c,
    X4ProbeHashContext *h,const atomic_bool *cancel)
{
    int rc=x4_live_video_probe_raw_start(v,pixels,count,c,h,cancel);
    if(!rc){((VideoState *)v->state)->Flush=runtime_empty_flush;((VideoState *)v->state)->Decode=runtime_decode;}
    return rc;
}
static void *runtime_calloc(size_t n,size_t bytes)
{
    void *p=calloc(n,bytes);
    ++runtime_allocations;runtime_context=p;return p;
}
static int runtime_finish_scanout(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease,
    uint64_t output,uint64_t decode_end,uint64_t converted)
{
    if(runtime_finish_rejected)return X4_PIPELINE_STATE;
    return x4_video_pipeline_finish_scanout(d,lease,output,decode_end,converted);
}
#define x4_live_video_probe_raw_start runtime_raw_start
#define x4_video_pipeline_finish_scanout runtime_finish_scanout
#define calloc runtime_calloc
#include "../src/video/probe_pipeline.c"
#undef calloc
#undef x4_video_pipeline_finish_scanout
#undef x4_live_video_probe_raw_start

static void runtime_assert_observation_count(void)
{
    Pipeline *p=runtime_context;CHECK(p);
    /* Called before returning each status. A previously returned stale arg
     * must not have advanced either presentation or the ownership ledger. */
    CHECK(p->presented==runtime_flip_observations);
    CHECK(p->display.ledger.observed==runtime_flip_observations);
    CHECK(p->display.ledger.pending==runtime_pending);
}

static void runtime_reset(void)
{
    reset_fixture();runtime_frames=12;runtime_source_at=0;runtime_cancel_tick=0;
    runtime_source_failure=runtime_open_failure=runtime_map_failure=runtime_close_failure=0;
    runtime_metadata_mismatch=false;runtime_source_opened=runtime_source_closed=0;
    runtime_flip_calls=runtime_flip_observations=0;runtime_pending=runtime_screen=-1;
    runtime_pending_arg=0;runtime_current_arg=runtime_previous_case_arg;
    runtime_stale_previous_polls=0;runtime_allocations=0;runtime_context=NULL;
    runtime_main_thread=pthread_self();runtime_source_next_calls=0;
    runtime_next_failure_at=runtime_decode_failure_at=UINT32_MAX;runtime_cancel_after_flip=false;
    runtime_incomplete_eof=false;runtime_finish_rejected=false;runtime_flip_delay=3;runtime_pending_polls=0;
    runtime_pending_pixel=runtime_screen_pixel=0;
    valid_output=true; x4_probe_stack_test_fail(0,0);
}
static X4ProbePipelineRunResult runtime_run(unsigned converters)
{
    runtime_descriptor.au_count=runtime_frames+(runtime_incomplete_eof?1:0);
    runtime_descriptor.duration_us=runtime_descriptor.au_count*UINT64_C(1000000)*runtime_descriptor.fps_den/runtime_descriptor.fps_num;
    printf("RUN runtime workers=%u frames=%u open_source=%d metadata=%u open_display=%d register=%d close=%d next_failure=%u decode_failure=%u cancel_tick=%u cancel_flip=%u incomplete=%u\n",
        converters,runtime_frames,runtime_source_failure,(unsigned)runtime_metadata_mismatch,runtime_open_failure,
        runtime_map_failure,runtime_close_failure,runtime_next_failure_at,runtime_decode_failure_at,
        runtime_cancel_tick,(unsigned)runtime_cancel_after_flip,(unsigned)runtime_incomplete_eof);
    X4Controller pad={0};return x4_probe_pipeline_run(runtime_case,X4_PROBE_MAX_SPEED,converters,&pad);
}
static void full_runs(void)
{
    for(unsigned n=0;n<3;++n) {
        unsigned workers=n==0?0:n+1;runtime_reset();
        X4ProbePipelineRunResult r=runtime_run(workers);
        CHECK(!r.stopped && !r.retained && !r.cancelled && r.report_result==0 && r.compatibility_passed);
        CHECK(r.status.source_eof && r.status.native_outputs==runtime_frames && r.status.input_aus==runtime_frames);
        CHECK(r.status.staging_bytes==0 && !reserved_count() && !runtime_video_open);
        CHECK(runtime_source_opened==1 && runtime_source_closed==1);
        CHECK(runtime_source_next_calls==runtime_frames+1);
        CHECK(workers?runtime_flip_calls>0:runtime_flip_calls==0);
        CHECK(runtime_flip_calls==runtime_flip_observations);
        if(workers==3)CHECK(runtime_stale_previous_polls==3);
        printf("PASS complete runtime workers=%u: EOF/drain, color fixture, leases, double/triple ownership and cleanup\n",workers);
    }
    puts("PASS stale flip status from previous case: three observations never advance counts or release pending buffer");
}
static void setup_failures(void)
{
    runtime_reset();runtime_source_failure=-902;
    X4ProbePipelineRunResult r=runtime_run(2);
    CHECK(r.stopped && !r.retained && !r.compatibility_passed && r.status.source_result==-902);
    CHECK(!query_calls && !create_calls && !reserved_count() && !runtime_video_open);
    runtime_reset();runtime_metadata_mismatch=true;r=runtime_run(2);
    CHECK(r.stopped && !r.retained && r.status.source_result==X4_PROBE_INDEX);
    CHECK(!query_calls && runtime_source_closed==1 && !reserved_count());
    runtime_reset();runtime_open_failure=-903;r=runtime_run(2);
    CHECK(r.stopped && !r.retained && !runtime_source_opened && !reserved_count());
    runtime_reset();runtime_map_failure=-904;r=runtime_run(2);
    CHECK(r.stopped && !r.retained && !runtime_source_opened && !reserved_count() && !runtime_video_open);
    runtime_reset();x4_probe_stack_test_fail(2,22);r=runtime_run(2);
    CHECK(r.stopped && !r.retained && !runtime_source_opened && !reserved_count() && !runtime_video_open);
    x4_probe_stack_test_fail(0,0);
    runtime_reset();runtime_cancel_tick=1;runtime_frames=300;r=runtime_run(2);
    CHECK(r.cancelled && !r.retained && !r.compatibility_passed && !reserved_count() && !runtime_video_open);
    runtime_reset();runtime_next_failure_at=3;r=runtime_run(2);
    CHECK(r.stopped && !r.retained && r.status.pipeline_result==-906 && r.status.input_aus==3 && !reserved_count());
    runtime_reset();runtime_decode_failure_at=3;r=runtime_run(2);
    CHECK(r.stopped && !r.retained && r.status.pipeline_result==-907 && r.status.decode_result==-907 && !reserved_count());
    runtime_reset();runtime_incomplete_eof=true;r=runtime_run(2);
    CHECK(r.stopped && !r.retained && !r.compatibility_passed && r.status.source_eof && !reserved_count());
    runtime_reset();runtime_frames=120;runtime_cancel_after_flip=true;r=runtime_run(3);
    CHECK(r.cancelled && !r.retained && !r.compatibility_passed && runtime_flip_calls && !reserved_count() && !runtime_video_open);
    runtime_reset();runtime_finish_rejected=true;r=runtime_run(2);
    CHECK(r.stopped && r.cancelled && !r.retained && !r.compatibility_passed);
    CHECK(!r.status.rgb_publications && !r.status.rgb_conversions && !runtime_flip_calls);
    CHECK(!reserved_count() && !runtime_video_open && runtime_source_closed==1);
    puts("PASS rejected scanout finish cancels without a false RGB publication or retained writer");
    puts("PASS runtime source/metadata/display/thread setup failures and user cancellation clean up safely");
}
static void retained_close(void)
{
    runtime_reset();runtime_close_failure=-905;
    X4ProbePipelineRunResult r=runtime_run(2);
    CHECK(r.stopped && r.retained && runtime_video_open && reserved_count()==1);
    Pipeline *p=runtime_context;CHECK(p && !p->decoder_thread.created);
    for(unsigned i=0;i<2;++i)CHECK(!p->workers[i].thread.created);
    CHECK(!p->video.state && p->display.memory);
    /* Test-only recovery of deliberately retained host resources. Production
     * never retries failed native close; it preserves the owner context. */
    runtime_close_failure=0;CHECK(scanout_close(&p->display)==0);
    free(p->reference_pixels);free(p->au[0]);free(p->au[1]);x4_probe_hash_context_close(&p->hash);
    x4_probe_metrics_free(p->metrics);free(p);runtime_context=NULL;
    CHECK(!reserved_count() && !runtime_video_open);
    puts("PASS runtime failed VideoOut close retains scanout allocation after all producers joined");
}
static void *jump_clock(void *argument)
{
    Pipeline *p=argument;
    for(;;) {
        ring_lock(p);uint64_t waits=p->ring.waits;ring_unlock(p);
        if(waits)break;
        sceKernelUsleep(100);
    }
    atomic_fetch_add(&fixture_clock_us,PIPE_TIMEOUT_US+1u);return NULL;
}
static void all_native_held(void)
{
    Pipeline *p=calloc(1,sizeof(*p));CHECK(p);
    atomic_flag_clear(&p->gate);atomic_init(&p->cancel,false);
    CHECK(x4_video_pipeline_ring_init(&p->ring,1)==0);
    X4VideoPipelineLease lease;
    for(unsigned i=0;i<4;++i)CHECK(x4_video_pipeline_acquire_decode(&p->ring,&lease,NULL)==0);
    pthread_t clock_thread;CHECK(pthread_create(&clock_thread,NULL,jump_clock,p)==0);
    CHECK(acquire_native(p,&lease)==X4_PIPELINE_EXHAUSTED);
    CHECK(pthread_join(clock_thread,NULL)==0 && p->ring.occupied==4 && !p->ring.discarded_ready);
    atomic_store(&p->cancel,true);CHECK(acquire_native(p,&lease)==X4_PROBE_CANCELLED);
    free(p);puts("PASS runtime four native-held outputs: controlled-clock timeout/cancel never steals buffers");
}
static void exhausted_flip_nonce(void)
{
    runtime_reset();Pipeline *p=calloc(1,sizeof(*p));CHECK(p);
    scanout_init(&p->display,1);
    X4VideoPipelineLease lease;
    CHECK(x4_video_pipeline_acquire_scanout(&p->display.ledger,&lease)==0);
    CHECK(x4_video_pipeline_finish_scanout(&p->display.ledger,lease,1,1,2)==0);
    uint64_t saved_nonce=next_pipeline_flip;
    next_pipeline_flip=(uint64_t)INT64_MAX;
    CHECK(display_tick(p)==X4_PIPELINE_EXHAUSTED && !runtime_flip_calls);
    CHECK(next_pipeline_flip==(uint64_t)INT64_MAX && p->display.ledger.pending<0);
    next_pipeline_flip=UINT64_MAX;
    CHECK(display_tick(p)==X4_PIPELINE_EXHAUSTED && !runtime_flip_calls);
    CHECK(next_pipeline_flip==UINT64_MAX && p->display.ledger.pending<0);
    /* Fixture-only restoration. Production never resets this process-wide
     * nonce and must stop before its cast to the signed native argument. */
    next_pipeline_flip=saved_nonce;
    CHECK(x4_video_pipeline_abort_scanout(&p->display.ledger,lease)==X4_PIPELINE_STATE);
    free(p);puts("PASS flip nonce signed-range exhaustion rejects before native Submit and never wraps");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    CHECK(argc==2);runtime_directory=argv[1];
    runtime_descriptor=x4_probe_cases[0];runtime_case=&runtime_descriptor;
    full_runs();setup_failures();retained_close();all_native_held();exhausted_flip_nonce();reset_fixture();
    puts("PASS runtime HOST MOCK; no native decoder, actual reference matching or PS4 throughput claim");return 0;
}
