/* SPDX-License-Identifier: GPL-3.0-only */
/* MOCK ONLY. Actual live_media local worker, source reader, mailbox, CPU draw
 * and close are exercised with real pthreads. Native decoder/trace/audio are
 * explicit doubles. Fixtures are not decodable H264 and their digest is not
 * SHA256. This cannot prove native decoding, four-reader safety or PS4 timing. */
#define _POSIX_C_SOURCE 200809L
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../src/media/live_media.c"
#pragma GCC diagnostic pop
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/video/probe_files.h"

static unsigned checks;
static void check(bool value, const char *expression, unsigned line)
{ ++checks; if (!value) { fprintf(stderr,"FAIL %u: %s\n",line,expression);exit(1); } }
#define CHECK(x) check(!!(x),#x,__LINE__)
static atomic_int create_failure, native_start_failure, native_stop_failure;
static atomic_int native_feed_failure, reference_close_failure;
static atomic_bool cancel_in_thread_create, block_hash, hash_entered;
static atomic_bool cancel_during_feed, cancel_during_flush, cancel_after_clean_publication;
static atomic_uint hash_calls, native_starts, native_stops, audio_calls;
static atomic_uint_fast64_t conversions_at_flush_cancel;
static bool fail_trace;
static atomic_uint hash_contexts_live;
static bool fail_hash_context;
int x4_probe_hash_context_init(X4ProbeHashContext *context)
{
    CHECK(context&&!context->buffer&&!context->capacity);
    if(fail_hash_context)return X4_PROBE_MEMORY;
    context->buffer=malloc(X4_PROBE_READ_CHUNK);if(!context->buffer)return X4_PROBE_MEMORY;
    context->capacity=X4_PROBE_READ_CHUNK;atomic_fetch_add(&hash_contexts_live,1);return 0;
}
void x4_probe_hash_context_close(X4ProbeHashContext *context)
{
    CHECK(context);
    if(context->buffer){free(context->buffer);atomic_fetch_sub(&hash_contexts_live,1);}
    context->buffer=NULL;context->capacity=0;
}

uint64_t sceKernelGetProcessTime(void)
{ struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) abort(); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
int sceKernelUsleep(unsigned us)
{ struct timespec t={(time_t)(us/1000000u),(long)(us%1000000u)*1000};while(nanosleep(&t,&t)&&errno==EINTR){} return 0; }
int scePthreadCreate(OrbisPthread *thread,const pthread_attr_t *attr,void *(*entry)(void *),void *arg,const char *name)
{
    (void)name;int rc=atomic_load(&create_failure);if(rc) return rc;
    if(atomic_load(&cancel_in_thread_create)) atomic_store(&((X4LiveMedia *)(attr ? ((X4ProbeThread *)arg)->argument : arg))->stop,true);
    return pthread_create(thread,attr,entry,arg);
}
int scePthreadJoin(OrbisPthread thread,void **result) { return pthread_join(thread,result); }
uint64_t x4_ingress_test_now_us(void) { return sceKernelGetProcessTime(); }
void x4_grace_inbox_test_sleep(unsigned us) { sceKernelUsleep(us); }
void x4_grace_inbox_test_after_enqueue_load(X4NackGraceInbox *inbox,uint_fast64_t at) { (void)inbox;(void)at; }
X4NackPacket x4_rtc_nack_delivery_ticket(void) { return (X4NackPacket){0}; }

struct X4Trace { X4ProbeMetrics *metrics;X4LiveMedia *owner;bool active,ended; };
uint64_t x4_trace_now_us(void) { return sceKernelGetProcessTime(); }
X4Trace *x4_trace_create(uint64_t when,unsigned readers) { (void)when;(void)readers;return fail_trace?NULL:calloc(1,sizeof(X4Trace)); }
void x4_trace_probe_metrics(X4Trace *t,X4ProbeMetrics *m) { if(t)t->metrics=m; }
void x4_trace_record(X4Trace *t,uint16_t e,uint16_t f,uint64_t a,uint64_t b)
{ if(t && t->metrics) x4_probe_metrics_record_event(t->metrics,sceKernelGetProcessTime(),e,f,a,b); }
void x4_trace_poll(X4Trace *t) { (void)t; }
void x4_trace_set_active(X4Trace *t,bool active) { if(t)t->active=active; }
void x4_trace_end_session(X4Trace *t) { if(t)t->ended=true; }
bool x4_trace_free(X4Trace *t) { if(t&&!t->ended)return false;free(t);return true; }
int x4_trace_dump_file(X4Trace *t,const char *p) { (void)t;(void)p;return 1; }
int x4_trace_monitor_dump(X4Trace *t,const char *p) { (void)t;(void)p;return 1; }
bool x4_trace_monitor_bind_queue(X4Trace *t,const X4VideoIngress *q) { (void)t;(void)q;return true; }
bool x4_trace_monitor_config(X4Trace *t,const X4MonitorConfig *c) { (void)t;(void)c;return true; }
int x4_trace_monitor_start(X4Trace *t) { (void)t;return 0; }
int x4_trace_monitor_stop(X4Trace *t) { (void)t;return 0; }
void x4_trace_monitor_state(X4Trace *t,unsigned a,unsigned p,uint64_t n)
{
    (void)a;(void)n;
    /* Controlled worker-owned boundary after a clean final RGB publication,
     * before the worker's final stop/error classification. No production hook
     * or state is changed; the double raises the same atomic cancellation flag. */
    if(t && t->owner && p==X4_MON_PHASE_ASSEMBLE &&
        atomic_load(&cancel_after_clean_publication) &&
        t->owner->probe_owner.finished && t->owner->probe_owner.pipeline_result==0)
        x4_live_media_probe_request_cancel(t->owner);
}
void x4_trace_monitor_epoch(X4Trace *t,unsigned a,unsigned e) { (void)t;(void)a;(void)e; }
void x4_trace_monitor_rx(X4Trace *t,unsigned k,bool v,uint32_t s,uint16_t q,size_t b,uint64_t n)
{ (void)t;(void)k;(void)v;(void)s;(void)q;(void)b;(void)n; }
void x4_trace_monitor_critical(X4Trace *t,uint64_t n,unsigned e,unsigned s,uint32_t f,uint64_t i,uint64_t src,uint64_t a,uint64_t b,uint64_t c)
{ (void)t;(void)n;(void)e;(void)s;(void)f;(void)i;(void)src;(void)a;(void)b;(void)c; }
X4LiveAudio *x4_live_audio_start(int *e,int pt) { (void)e;(void)pt;atomic_fetch_add(&audio_calls,1);return NULL; }
void x4_live_audio_receive(X4LiveAudio *a,const uint8_t *p,size_t n) { (void)a;(void)p;(void)n;atomic_fetch_add(&audio_calls,1); }
void x4_live_audio_snapshot(const X4LiveAudio *a,X4LiveAudioSnapshot *s) { (void)a;memset(s,0,sizeof(*s));atomic_fetch_add(&audio_calls,1); }
void x4_live_audio_mute(X4LiveAudio *a,bool b) { (void)a;(void)b;atomic_fetch_add(&audio_calls,1); }
int x4_live_audio_stop(X4LiveAudio *a) { (void)a;atomic_fetch_add(&audio_calls,1);return 0; }

typedef struct { const X4ProbeCase *sample;atomic_bool *cancel;bool pending;uint64_t value; } MockVideo;
int x4_live_video_probe_start(X4LiveVideo *v,uint32_t *pixels,size_t capacity,const X4ProbeCase *sample,X4ProbeHashContext *hash_context,const atomic_bool *cancel)
{
    if(!hash_context||!hash_context->buffer||hash_context->capacity!=X4_PROBE_READ_CHUNK)abort();
    (void)capacity;atomic_fetch_add(&native_starts,1);
    MockVideo *state=calloc(1,sizeof(*state));if(!state) return -5;
    /* The fixture intentionally drives cancellation inside a fake native
     * callback. The original pointer is the mutable media stop atomic. */
    state->sample=sample;state->cancel=(atomic_bool *)cancel;v->state=state;v->pixels=pixels;
    v->probe_status=x4_probe_status_pending();
    v->probe_status.query_result=0;v->probe_status.reference_result=0;
    v->probe_status.create_result=atomic_load(&native_start_failure);
    v->probe_status.layout_verdict=X4_PROBE_LAYOUT_VERIFIED;
    return atomic_load(&native_start_failure);
}
int x4_live_video_probe_start_memory(X4LiveVideo *v,uint32_t *pixels,size_t capacity,
    const X4ProbeCase *sample,X4ProbeHashContext *hash_context,const atomic_bool *cancel,
    int output_memory_type,X4ProbeMetrics *metrics)
{
    (void)metrics;
    if(output_memory_type!=0 && output_memory_type!=3) return -7;
    return x4_live_video_probe_start(v,pixels,capacity,sample,hash_context,cancel);
}
int x4_live_video_start(X4LiveVideo *v,uint32_t *p,size_t c) { (void)v;(void)p;(void)c;abort(); }
int x4_live_video_set_target(X4LiveVideo *v,uint32_t *p,size_t c) { (void)c;if(!v->state)return -7;v->pixels=p;return 0; }
int x4_live_video_feed(X4LiveVideo *v,const uint8_t *p,size_t bytes,uint64_t pts)
{
    (void)p;(void)bytes;(void)pts;MockVideo *s=v->state;if(!s)abort();
    if(s->pending)x4_trace_record(v->trace,X4_TRACE_OUTPUT_SUPERSEDED,0,s->value,s->value+1);
    x4_trace_record(v->trace,X4_TRACE_DECODE_BEGIN,0,++v->trace_decode_next,v->trace_au);
    ++v->decode_calls;int result=atomic_load(&native_feed_failure);v->probe_status.decode_result=result;
    if(atomic_load(&cancel_during_feed))atomic_store(s->cancel,true);
    if(result){x4_trace_record(v->trace,X4_TRACE_DECODE_END,0,v->trace_decode_next,(uint32_t)result);return result;}
    ++v->decoded_frames;++v->probe_status.native_outputs;s->value=v->decoded_frames;s->pending=true;
    x4_trace_record(v->trace,X4_TRACE_DECODE_END,3,v->trace_decode_next,0);
    x4_trace_record(v->trace,X4_TRACE_OUTPUT_VALID,0,s->value,0);return 0;
}
int x4_live_video_convert_pending(X4LiveVideo *v)
{
    MockVideo *s=v->state;if(!s||!s->pending)return 0;
    for(size_t i=0;i<(size_t)s->sample->visible_width*s->sample->visible_height;++i)
        v->pixels[i]=0xff000000u|(uint32_t)s->value;
    s->pending=false;v->width=s->sample->visible_width;v->height=s->sample->visible_height;
    ++v->frames;v->trace_converted_output=s->value;v->last_picture_time_us=sceKernelGetProcessTime();return 0;
}
int x4_live_video_probe_flush(X4LiveVideo *v)
{
    MockVideo *s=v->state;if(!s)abort();
    int result=atomic_load(&cancel_during_flush)?X4_PROBE_CANCELLED:0;
    v->probe_status.flush_result=result;
    if(result==X4_PROBE_CANCELLED){atomic_store(&conversions_at_flush_cancel,v->frames);atomic_store(s->cancel,true);}
    return result;
}
int x4_live_video_stop(X4LiveVideo *v)
{
    atomic_fetch_add(&native_stops,1);int result=atomic_load(&native_stop_failure);
    if(result)return result;
    if(v->state){v->probe_status.delete_decoder_result=0;v->probe_status.release_queue_result=0;
        v->probe_status.reference_close_result=atomic_load(&reference_close_failure);}
    free(v->state);v->state=NULL;v->pixels=NULL;return 0;
}

/* A fixture-only digest, distinct from the native MbedTLS binding. */
int x4_probe_hash_file(FILE *file,uint64_t bytes,uint8_t actual[32],const atomic_bool *cancel,void *context)
{
    if(cancel){X4ProbeHashContext *hash=context;if(!hash||!hash->buffer||hash->capacity!=X4_PROBE_READ_CHUNK)abort();}
    unsigned call=atomic_fetch_add(&hash_calls,1)+1;
    if(atomic_load(&block_hash) && call==2) {
        atomic_store(&hash_entered,true);
        uint64_t begin=sceKernelGetProcessTime();
        while(cancel && !atomic_load(cancel)) { if(sceKernelGetProcessTime()-begin>5000000) return -4;sceKernelUsleep(100); }
        return X4_PROBE_CANCELLED;
    }
    uint64_t value=UINT64_C(0xcbf29ce484222325),total=0;uint8_t block[256];
    while(total<bytes) {
        if(cancel && atomic_load(cancel))return X4_PROBE_CANCELLED;
        size_t want=bytes-total<sizeof(block)?(size_t)(bytes-total):sizeof(block);
        if(fread(block,1,want,file)!=want)return -4;
        for(size_t i=0;i<want;++i){value^=block[i];value*=UINT64_C(0x100000001b3);}total+=want;
    }
    for(unsigned i=0;i<32;++i)actual[i]=(uint8_t)(value>>((i&7u)*8));
    return 0;
}
typedef struct { char dir[80],samples[112],clip[128],index[128];X4ProbeCase sample; } Fixture;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(i*8));}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(n>>(i*8));}
static void reset_doubles(void)
{
    atomic_store(&create_failure,0);atomic_store(&native_start_failure,0);atomic_store(&native_stop_failure,0);
    atomic_store(&native_feed_failure,0);atomic_store(&reference_close_failure,0);
    atomic_store(&cancel_in_thread_create,false);atomic_store(&block_hash,false);atomic_store(&hash_entered,false);
    atomic_store(&cancel_during_feed,false);atomic_store(&cancel_during_flush,false);atomic_store(&cancel_after_clean_publication,false);
    atomic_store(&conversions_at_flush_cancel,0);
    atomic_store(&hash_calls,0);atomic_store(&native_starts,0);atomic_store(&native_stops,0);atomic_store(&audio_calls,0);fail_trace=false;
}
static void fixture_create(Fixture *f)
{
    memset(f,0,sizeof(*f));strcpy(f->dir,"/tmp/x4-probe-media-mock.XXXXXX");CHECK(mkdtemp(f->dir));
    CHECK(!x4_probe_files_test_root(f->dir));
    CHECK(snprintf(f->samples,sizeof(f->samples),"%s/xcloud4-probe",f->dir)>0);CHECK(!mkdir(f->samples,0700));
    CHECK(snprintf(f->clip,sizeof(f->clip),"%s/tiny.h264",f->samples)>0);
    CHECK(snprintf(f->index,sizeof(f->index),"%s/tiny.x4au",f->samples)>0);
    f->sample=(X4ProbeCase){.id="MOCK",.clip_path=f->clip,.index_path=f->index,
        .visible_width=2,.visible_height=2,.coded_width=2,.coded_height=2,
        .fps_num=60,.fps_den=1,.au_count=3,.duration_us=50000,.chosen_dpb_frames=4};
    uint8_t clip[60];for(unsigned i=0;i<sizeof(clip);++i)clip[i]=(uint8_t)(i*19+7);
    for(unsigned i=0;i<3;++i){clip[i*20]=clip[i*20+1]=clip[i*20+2]=0;clip[i*20+3]=1;clip[i*20+4]=9;}
    FILE *file=fopen(f->clip,"wb");CHECK(file);CHECK(fwrite(clip,1,sizeof(clip),file)==sizeof(clip));CHECK(!fclose(file));
    file=fopen(f->clip,"rb");CHECK(file);CHECK(!x4_probe_hash_file(file,60,f->sample.clip_sha256,NULL,NULL));CHECK(!fclose(file));
    uint8_t index[168]={0};memcpy(index,"X4AU1\0\0\0",8);put32(index+8,1);put32(index+12,96);
    put64(index+16,60);put64(index+24,3);put64(index+32,50000);
    put32(index+40,2);put32(index+44,2);put32(index+48,2);put32(index+52,2);put32(index+56,60);put32(index+60,1);
    memcpy(index+64,f->sample.clip_sha256,32);
    for(unsigned i=0;i<3;++i){put64(index+96+i*24,i*20);put64(index+104+i*24,20);put64(index+112+i*24,(uint64_t)i*1000000/60);}
    file=fopen(f->index,"wb");CHECK(file);CHECK(fwrite(index,1,sizeof(index),file)==sizeof(index));CHECK(!fclose(file));
    file=fopen(f->index,"rb");CHECK(file);CHECK(!x4_probe_hash_file(file,sizeof(index),f->sample.index_sha256,NULL,NULL));CHECK(!fclose(file));
    reset_doubles();
}
static void fixture_remove(Fixture *f){CHECK(!unlink(f->clip));CHECK(!unlink(f->index));CHECK(!rmdir(f->samples));CHECK(!rmdir(f->dir));}
static X4LiveMedia *create(Fixture *f,X4ProbeMetrics **metrics)
{
    *metrics=x4_probe_metrics_create(sceKernelGetProcessTime());CHECK(*metrics);
    int error=-999;X4LiveMedia *m=x4_live_media_probe_create(&f->sample,X4_PROBE_MAX_SPEED,*metrics,&error);
    CHECK(m&&error==0);CHECK(m->probe_hash.buffer&&m->probe_hash.capacity==X4_PROBE_READ_CHUNK);
    m->trace->owner=m;return m;
}
static void wait_done(X4LiveMedia *m)
{
    uint64_t begin=sceKernelGetProcessTime();X4ProbeStatus status;
    while(!x4_live_media_probe_owner_done(m)) {
        CHECK(!x4_live_media_probe_snapshot(m,&status));
        CHECK(sceKernelGetProcessTime()-begin<5000000);sceKernelUsleep(100);
    }
}
static void eof_case(Fixture *f)
{
    X4ProbeMetrics *metrics;X4LiveMedia *m=create(f,&metrics);CHECK(!x4_live_media_start(m));wait_done(m);
    X4ProbeStatus status;CHECK(!x4_live_media_probe_snapshot(m,&status));
    CHECK(status.source_eof && status.finished && status.pipeline_result==0 && status.close_result==0);
    CHECK(status.input_aus==3 && status.native_outputs==3 && status.rgb_conversions==1 && status.rgb_publications==1);
    CHECK(!m->video.state && !m->probe_source.state && m->rgb[m->published_slot].pixels);
    CHECK(atomic_load(&native_starts)==1 && atomic_load(&native_stops)==1 && !atomic_load(&audio_calls));
    CHECK(x4_live_media_has_new_picture(m));
    uint32_t *framebuffer=calloc((size_t)X4_WIDTH*X4_HEIGHT,sizeof(*framebuffer));CHECK(framebuffer);
    CHECK(x4_live_media_draw(m,framebuffer)==1);CHECK(framebuffer[420]==0xff000003u);
    x4_live_media_note_present(m,17);CHECK(!x4_live_media_has_new_picture(m));free(framebuffer);
    CHECK(!x4_live_media_close(m));x4_probe_metrics_end(metrics,sceKernelGetProcessTime(),1);
    const X4ProbeStackReport *stack=x4_probe_metrics_stack(metrics,0);
    CHECK(stack&&stack->configured_bytes==262144&&stack->reported_bytes==262144);
    CHECK(stack->started&&stack->finished&&stack->pattern_ready&&stack->join_rc==0);
    X4ProbeMetricsSummary summary;CHECK(!x4_probe_metrics_summary(metrics,&summary));
    CHECK(summary.total[X4_PROBE_COUNT_INPUT_AU]==3 && summary.total[X4_PROBE_COUNT_NATIVE_OUTPUT]==3);
    CHECK(summary.total[X4_PROBE_COUNT_RGB_PUBLICATION]==1);x4_probe_metrics_free(metrics);
}
static void failure_cases(Fixture *f)
{
    X4ProbeMetrics *metrics;X4LiveMedia *m=create(f,&metrics);x4_live_media_probe_request_cancel(m);
    CHECK(x4_live_media_start(m)==X4_LIVE_ERR_STATE && x4_live_media_probe_owner_done(m));
    CHECK(!atomic_load(&hash_calls)&&!atomic_load(&native_starts));CHECK(!x4_live_media_close(m));x4_probe_metrics_free(metrics);
    reset_doubles();m=create(f,&metrics);atomic_store(&create_failure,EAGAIN);
    CHECK(x4_live_media_start(m)==-EAGAIN && x4_live_media_probe_owner_done(m));
    CHECK(!x4_live_media_close(m));
    const X4ProbeStackReport *stack=x4_probe_metrics_stack(metrics,0);
    CHECK(stack&&stack->create_rc==EAGAIN&&!stack->started);
    x4_probe_metrics_free(metrics);
    reset_doubles();m=create(f,&metrics);atomic_store(&cancel_in_thread_create,true);
    CHECK(!x4_live_media_start(m));wait_done(m);CHECK(!atomic_load(&hash_calls)&&!atomic_load(&native_starts));
    X4ProbeStatus status;CHECK(!x4_live_media_probe_snapshot(m,&status));CHECK(status.pipeline_result==X4_PROBE_CANCELLED);
    CHECK(status.query_result==X4_PROBE_RC_NOT_RUN && status.delete_decoder_result==X4_PROBE_RC_NOT_RUN &&
        status.flush_result==X4_PROBE_RC_NOT_RUN && status.close_result==0);
    CHECK(!x4_live_media_close(m));x4_probe_metrics_free(metrics);
    reset_doubles();m=create(f,&metrics);atomic_store(&block_hash,true);CHECK(!x4_live_media_start(m));
    uint64_t begin=sceKernelGetProcessTime();while(!atomic_load(&hash_entered)) { CHECK(sceKernelGetProcessTime()-begin<5000000);sceKernelUsleep(100); }
    CHECK(!x4_live_media_probe_snapshot(m,&status));x4_live_media_probe_request_cancel(m);wait_done(m);
    CHECK(!atomic_load(&native_starts) && !m->probe_source.state);CHECK(!x4_live_media_close(m));x4_probe_metrics_free(metrics);
    reset_doubles();m=create(f,&metrics);atomic_store(&native_start_failure,-88);CHECK(!x4_live_media_start(m));wait_done(m);
    CHECK(!x4_live_media_probe_snapshot(m,&status));CHECK(status.query_result==0&&status.create_result==-88&&status.pipeline_result==-88);
    CHECK(!m->video.state&&!m->probe_source.state);CHECK(!x4_live_media_close(m));x4_probe_metrics_free(metrics);
}
static void retained_teardown(Fixture *f)
{
    reset_doubles();X4ProbeMetrics *metrics;X4LiveMedia *m=create(f,&metrics);atomic_store(&native_stop_failure,-77);
    CHECK(!x4_live_media_start(m));wait_done(m);CHECK(m->video.state && !m->probe_source.state);
    CHECK(x4_live_media_close(m)==-77 && m->rgb[m->published_slot].pixels && m->trace);
    CHECK(x4_live_media_close(m)==-77 && atomic_load(&native_stops)==1);
    /* Fixture-only reclamation of a synthetic malloc object, AFTER assertions
     * and owner join. Never a prescription to retry/free failed PS4 resources. */
    free(m->video.state);m->video.state=NULL;m->worker_close_result=0;
    CHECK(!x4_live_media_close(m));x4_probe_metrics_free(metrics);
}
static void end_reason_cases(void)
{
    CHECK(x4_probe_end_reason(NULL)==X4_PROBE_END_SETUP_FAILURE);
    X4ProbeStatus s=x4_probe_status_pending();
    CHECK(x4_probe_end_reason(&s)==X4_PROBE_END_ERROR); /* Unexecuted is not clean EOF. */
    s.pipeline_result=0;s.source_eof=true;s.close_result=0;
    CHECK(x4_probe_end_reason(&s)==X4_PROBE_END_CLEAN_EOF);
    s.cancel_requested=true;
    CHECK(x4_probe_end_reason(&s)==X4_PROBE_END_CLEAN_EOF); /* Late stop is only a flag. */
    s.pipeline_result=X4_PROBE_CANCELLED;
    CHECK(x4_probe_end_reason(&s)==X4_PROBE_END_CANCEL);
    s.pipeline_result=-91;
    CHECK(x4_probe_end_reason(&s)==X4_PROBE_END_ERROR);
    s.pipeline_result=0;s.reference_close_result=-92;
    CHECK(x4_probe_end_reason(&s)==X4_PROBE_END_ERROR);
    s.reference_close_result=0;s.close_result=-77;
    CHECK(x4_probe_end_reason(&s)==X4_PROBE_END_ERROR);
}
static void cancellation_result_cases(Fixture *f)
{
    reset_doubles();X4ProbeMetrics *metrics;X4LiveMedia *m=create(f,&metrics);
    atomic_store(&cancel_during_flush,true);CHECK(!x4_live_media_start(m));wait_done(m);
    X4ProbeStatus status;CHECK(!x4_live_media_probe_snapshot(m,&status));
    CHECK(status.source_eof && status.finished && status.cancel_requested && !status.draining);
    CHECK(status.pipeline_result==X4_PROBE_CANCELLED && status.flush_result==X4_PROBE_CANCELLED);
    CHECK(status.decode_result==0 && status.close_result==0 && status.input_aus==3);
    CHECK(status.rgb_conversions==atomic_load(&conversions_at_flush_cancel) &&
        status.rgb_publications==status.rgb_conversions && !m->video.error);
    CHECK(x4_probe_end_reason(&status)==X4_PROBE_END_CANCEL);
    CHECK(!m->video.state && !m->probe_source.state);CHECK(!x4_live_media_close(m));
    x4_probe_metrics_end(metrics,sceKernelGetProcessTime(),X4_PROBE_END_CANCEL);
    X4ProbeMetricsSummary summary;CHECK(!x4_probe_metrics_summary(metrics,&summary));
    CHECK(summary.total[X4_PROBE_COUNT_CANCEL]==1 && !summary.total[X4_PROBE_COUNT_ERRORS]);
    x4_probe_metrics_free(metrics);

    reset_doubles();m=create(f,&metrics);atomic_store(&native_feed_failure,-91);
    atomic_store(&cancel_during_feed,true);CHECK(!x4_live_media_start(m));wait_done(m);
    CHECK(!x4_live_media_probe_snapshot(m,&status));
    CHECK(status.pipeline_result==-91 && status.decode_result==-91 && m->video.error==-91);
    CHECK(status.cancel_requested && status.finished && status.close_result==0 && status.input_aus==1);
    CHECK(status.flush_result==X4_PROBE_RC_NOT_RUN && !status.source_eof);
    CHECK(!status.native_outputs && !status.rgb_conversions && !status.rgb_publications);
    CHECK(x4_probe_end_reason(&status)==X4_PROBE_END_ERROR);
    CHECK(!x4_live_media_close(m));x4_probe_metrics_end(metrics,sceKernelGetProcessTime(),X4_PROBE_END_ERROR);
    CHECK(!x4_probe_metrics_summary(metrics,&summary));
    /* Decode's trace and the owner each observe the failure; this is not a
     * count of distinct decoder failures. Cancellation must not mask either. */
    CHECK(summary.total[X4_PROBE_COUNT_ERRORS]>=1 && !summary.total[X4_PROBE_COUNT_CANCEL]);
    x4_probe_metrics_free(metrics);

    reset_doubles();m=create(f,&metrics);atomic_store(&cancel_after_clean_publication,true);
    CHECK(!x4_live_media_start(m));wait_done(m);CHECK(!x4_live_media_probe_snapshot(m,&status));
    CHECK(status.cancel_requested && status.source_eof && status.finished);
    CHECK(status.pipeline_result==0 && status.flush_result==0 && status.close_result==0);
    CHECK(status.rgb_publications>=1 && x4_live_media_has_new_picture(m));
    CHECK(x4_probe_end_reason(&status)==X4_PROBE_END_CLEAN_EOF);
    /* A subsequent UI stop after owner_done must also preserve its final result. */
    x4_live_media_probe_request_cancel(m);CHECK(!x4_live_media_probe_snapshot(m,&status));
    CHECK(status.pipeline_result==0 && x4_probe_end_reason(&status)==X4_PROBE_END_CLEAN_EOF);
    CHECK(!x4_live_media_close(m));x4_probe_metrics_end(metrics,sceKernelGetProcessTime(),X4_PROBE_END_CLEAN_EOF);
    CHECK(!x4_probe_metrics_summary(metrics,&summary));
    CHECK(!summary.total[X4_PROBE_COUNT_CANCEL] && !summary.total[X4_PROBE_COUNT_ERRORS]);
    x4_probe_metrics_free(metrics);

    reset_doubles();m=create(f,&metrics);atomic_store(&reference_close_failure,-92);
    CHECK(!x4_live_media_start(m));wait_done(m);CHECK(!x4_live_media_probe_snapshot(m,&status));
    CHECK(status.source_eof && status.pipeline_result==0 && status.close_result==0);
    CHECK(status.reference_close_result==-92 && status.delete_decoder_result==0 && status.release_queue_result==0);
    CHECK(!m->video.state && !m->probe_source.state && x4_probe_end_reason(&status)==X4_PROBE_END_ERROR);
    CHECK(!x4_live_media_close(m));x4_probe_metrics_end(metrics,sceKernelGetProcessTime(),X4_PROBE_END_ERROR);
    CHECK(!x4_probe_metrics_summary(metrics,&summary));
    CHECK(summary.total[X4_PROBE_COUNT_ERRORS]==1 && !summary.total[X4_PROBE_COUNT_CANCEL]);
    x4_probe_metrics_free(metrics);
    CHECK(!atomic_load(&audio_calls));
}
int main(void)
{
    Fixture fixture;fixture_create(&fixture);end_reason_cases();eof_case(&fixture);reset_doubles();failure_cases(&fixture);
    retained_teardown(&fixture);cancellation_result_cases(&fixture);
    reset_doubles();X4ProbeMetrics *metrics=x4_probe_metrics_create(sceKernelGetProcessTime());CHECK(metrics);fail_trace=true;int error=0;
    CHECK(!x4_live_media_probe_create(&fixture.sample,X4_PROBE_MAX_SPEED,metrics,&error)&&error==X4_LIVE_ERR_MEMORY);
    CHECK(!atomic_load(&native_starts));x4_probe_metrics_free(metrics);
    fail_trace=false;fail_hash_context=true;metrics=x4_probe_metrics_create(sceKernelGetProcessTime());CHECK(metrics);
    CHECK(!x4_live_media_probe_create(&fixture.sample,X4_PROBE_MAX_SPEED,metrics,&error)&&error==X4_LIVE_ERR_MEMORY);
    CHECK(!atomic_load(&native_starts));x4_probe_metrics_free(metrics);fail_hash_context=false;fixture_remove(&fixture);
    CHECK(!atomic_load(&hash_contexts_live));
    printf("PASS MOCK media lifecycle checks=%u; actual local worker/source/mailbox/draw, fake native/trace/audio; NOT PS4\n",checks);return 0;
}
