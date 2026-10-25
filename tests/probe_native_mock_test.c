/* SPDX-License-Identifier: GPL-3.0-only */
/* Deterministic HOST native-call fixture of the actual production implementation.
 * Every module/kernel/decoder function below is a mock. This is call ordering,
 * bounds and retention coverage, NOT PS4 codec/layout/concurrency evidence. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include "../src/video/live_h264.c"
#if X4_VIDEO_PROBE
#include "../src/video/probe_samples.generated.h"
#include "../src/video/probe_metrics.h"
#endif
static atomic_uint checks;
static unsigned query_calls, create_calls, decode_calls, flush_calls, delete_calls, release_calls, join_calls;
static int query_rc, create_rc, decode_rc, flush_rc, delete_rc, release_rc, join_rc;
static bool valid_output;
static unsigned output_height_override, output_pitch_override, output_picture_count, reference_matches;
static int reference_open_rc, reference_close_rc;
static size_t query_cpu_bytes, query_gpu_bytes, query_frame_extra;
static bool reject_cacheable_output_allocate;
static atomic_uint_fast64_t clock_us;
#include <time.h>
static OrbisVideodec2DecoderConfigInfo seen_config;
static OrbisVideodec2DecoderConfigInfo created_config;
#if defined(X4_NATIVE_MOCK_STREAM_FAULTS)
/* Test-only scripted outcomes; ordinary probe/legacy fixtures are unchanged. */
static int (*stream_query_fault)(unsigned, const OrbisVideodec2DecoderConfigInfo *);
static int (*stream_create_fault)(unsigned, const OrbisVideodec2DecoderConfigInfo *);
static OrbisVideodec2DecoderConfigInfo stream_query_configs[3], stream_create_configs[3];
static bool stream_create_success_null;
#endif
typedef struct { void *memory; size_t size; int type; bool reserved, mapped; } Reservation;
static Reservation reservations[32];
static unsigned next_reservation;
#define CHECK(x) do { ++checks; assert(x); } while (0)
uint64_t sceKernelGetProcessTime(void) { return ++clock_us; }
int sceKernelUsleep(unsigned us) { clock_us += us; struct timespec t={0,(long)us*1000}; return nanosleep(&t,NULL); }
size_t sceKernelGetDirectMemorySize(void) { return 256u * 1024u * 1024u; }
int sceKernelAllocateDirectMemory(off_t from, off_t to, size_t bytes, size_t alignment, int type, off_t *offset)
{
    (void)from; (void)to; CHECK(next_reservation + 1 < 32);
    size_t frame_bytes=((size_t)seen_config.maxFrameWidth*seen_config.maxFrameHeight*3u/2u+alignment-1u)&~(alignment-1u);
    if(reject_cacheable_output_allocate && type==0 && bytes==frame_bytes) return -818;
    unsigned i = ++next_reservation; void *memory = NULL;
    CHECK(posix_memalign(&memory, alignment, bytes) == 0);
    reservations[i] = (Reservation){.memory = memory, .size = bytes, .type = type, .reserved = true}; *offset = i; return 0;
}
int sceKernelMapDirectMemory(void **out, size_t bytes, int prot, int flags, off_t offset, size_t alignment)
{
    (void)prot; (void)flags; (void)alignment; CHECK(offset > 0 && offset < 32);
    CHECK(reservations[offset].reserved && reservations[offset].size == bytes);
    *out = reservations[offset].memory; reservations[offset].mapped = true; return 0;
}
int sceKernelMunmap(void *memory, size_t bytes)
{
    for (unsigned i = 1; i <= next_reservation; ++i)
        if (reservations[i].memory == memory) {
            CHECK(reservations[i].mapped && reservations[i].size == bytes);
            reservations[i].mapped = false; return 0;
        }
    CHECK(false); return -1;
}
int sceKernelReleaseDirectMemory(off_t offset, size_t bytes)
{
    ++release_calls; CHECK(offset > 0 && offset < 32);
    CHECK(reservations[offset].reserved && !reservations[offset].mapped && reservations[offset].size == bytes);
    if (release_rc) return release_rc;
    free(reservations[offset].memory); reservations[offset].memory = NULL;
    reservations[offset].reserved = false; return 0;
}
int scePthreadCreate(OrbisPthread *t, const pthread_attr_t *a, void *(*f)(void *), void *p, const char *name)
{ (void)name; if(a) return pthread_create(t,a,f,p); return 11; /* Ordinary streaming fixture retains serial fallback. */ }
int scePthreadJoin(OrbisPthread t, void **out)
{ ++join_calls; return join_rc ? join_rc : pthread_join(t,out); }
void x4_trace_record(X4Trace *t, uint16_t e, uint16_t f, uint64_t a, uint64_t b)
{ (void)t; (void)e; (void)f; (void)a; (void)b; }
static int32_t query_compute(OrbisVideodec2ComputeMemoryInfo *m)
{ m->cpuGpuMemorySize = 16384; return 0; }
static int32_t alloc_queue(const OrbisVideodec2ComputeConfigInfo *c, const OrbisVideodec2ComputeMemoryInfo *m, void **out)
{ (void)c; (void)m; *out = (void *)(uintptr_t)0x11; return 0; }
static int32_t release_queue(void *q) { (void)q; return 0; }
static int32_t query_decoder(const OrbisVideodec2DecoderConfigInfo *c, OrbisVideodec2DecoderMemoryInfo *m)
{
    ++query_calls; seen_config = *c;
#if defined(X4_NATIVE_MOCK_STREAM_FAULTS)
    if(query_calls<=3)stream_query_configs[query_calls-1]=*c;
    if(stream_query_fault){int rc=stream_query_fault(query_calls,c);if(rc)return rc;}
#endif
    if (query_rc) return query_rc;
    m->cpuMemorySize = query_cpu_bytes; m->gpuMemorySize = query_gpu_bytes; m->cpuGpuMemorySize = 16384;
    m->maxFrameBufferSize = (size_t)c->maxFrameWidth * c->maxFrameHeight * 3u / 2u + query_frame_extra;
    m->frameBufferAlignment = 16384; return 0;
}
static int32_t create_decoder(const OrbisVideodec2DecoderConfigInfo *c, const OrbisVideodec2DecoderMemoryInfo *m, void **out)
{
    created_config=*c; (void)m; ++create_calls;
#if defined(X4_NATIVE_MOCK_STREAM_FAULTS)
    if(create_calls<=3)stream_create_configs[create_calls-1]=*c;
    if(stream_create_fault){int rc=stream_create_fault(create_calls,c);if(rc)return rc;}
#endif
    if (create_rc) return create_rc;
#if defined(X4_NATIVE_MOCK_STREAM_FAULTS)
    if(stream_create_success_null){*out=NULL;return 0;}
#endif
    *out = (void *)(uintptr_t)0x22; return 0;
}
static int32_t delete_decoder(void *d) { (void)d; ++delete_calls; return delete_rc; }
static void output_picture(OrbisVideodec2FrameBuffer *f, OrbisVideodec2OutputInfo *o)
{
    f->isAccepted = true; if (!valid_output) return;
    size_t y = (size_t)seen_config.maxFrameWidth * seen_config.maxFrameHeight;
    memset(f->pFrameBuffer, 16, y); memset((uint8_t *)f->pFrameBuffer + y, 128, y / 2u);
    o->isValid = true; o->pictureCount = output_picture_count; o->codecType = 1;
    o->frameWidth = (unsigned)seen_config.maxFrameWidth;
    o->frameHeight = output_height_override ? output_height_override : (unsigned)seen_config.maxFrameHeight;
    o->framePitch = output_pitch_override ? output_pitch_override : (unsigned)seen_config.maxFrameWidth;
    o->pFrameBuffer = f->pFrameBuffer; o->frameBufferSize = f->frameBufferSize;
}
static int32_t decode(void *d, const OrbisVideodec2InputData *in, OrbisVideodec2FrameBuffer *f, OrbisVideodec2OutputInfo *o)
{ (void)d; CHECK(in->pAuData != NULL && in->auSize > 0); ++decode_calls; if (decode_rc) return decode_rc; output_picture(f, o); return 0; }
static int32_t flush(void *d, OrbisVideodec2FrameBuffer *f, OrbisVideodec2OutputInfo *o)
{ (void)d; ++flush_calls; if (flush_rc) return flush_rc; output_picture(f, o); return 0; }
int x4_module_open(const char *name) { CHECK(strcmp(name, "libSceVideodec2") == 0); return 1; }
int x4_module_symbol(int handle, const char *name, void **out)
{
    CHECK(handle == 1);
#define SYMBOL(n, fn) if (strcmp(name, "sceVideodec2" n) == 0) { *out = (void *)(fn); return 0; }
    SYMBOL("QueryComputeMemoryInfo", query_compute) SYMBOL("AllocateComputeQueue", alloc_queue)
    SYMBOL("ReleaseComputeQueue", release_queue) SYMBOL("QueryDecoderMemoryInfo", query_decoder)
    SYMBOL("CreateDecoder", create_decoder) SYMBOL("DeleteDecoder", delete_decoder)
    SYMBOL("Decode", decode) SYMBOL("Flush", flush)
#undef SYMBOL
    return -1;
}
#if X4_VIDEO_PROBE
/* Timing integration only: this fixture does not claim real PS4 durations. */
static atomic_uint observed_copy_stages[X4_PROBE_STAGE_COUNT];
void x4_probe_metrics_record_duration(X4ProbeMetrics *metrics, X4ProbeStage stage_id,
    uint64_t now, uint64_t duration)
{
    (void)now; (void)duration;
    if(metrics) { CHECK(stage_id<X4_PROBE_STAGE_COUNT); ++observed_copy_stages[stage_id]; }
}
/* No SHA implementation runs in this native-order mock. This sentinel only
 * verifies the prepared context passes unchanged to reference verification. */
static uint8_t hash_sentinel;
static X4ProbeHashContext hash_context={&hash_sentinel,X4_PROBE_READ_CHUNK};
int x4_probe_hash_file(FILE *f, uint64_t n, uint8_t hash[32], const atomic_bool *cancel, void *context)
{ (void)f; (void)n; (void)cancel; (void)context; memset(hash, 0, 32); return 0; }
int x4_probe_reference_open_verified(X4ProbeReference *r, const char *p, unsigned w, unsigned h, unsigned n,
    const uint8_t hash[32], X4ProbeHashFile fn, void *ctx, const atomic_bool *cancel)
{ (void)p; (void)hash; (void)fn;
#if defined(X4_NATIVE_MOCK_RUNTIME)
    CHECK(ctx && ((X4ProbeHashContext *)ctx)->buffer && ((X4ProbeHashContext *)ctx)->capacity==X4_PROBE_READ_CHUNK);
#else
    CHECK(ctx==&hash_context);
#endif
    (void)cancel; r->width = w; r->height = h; r->count = n; return reference_open_rc; }
int x4_probe_reference_match(X4ProbeReference *r, const uint32_t *p, size_t n, unsigned w, unsigned h, X4ProbeReferenceScore *score)
{ (void)r; (void)p; (void)n; (void)w; (void)h; memset(score, 0, sizeof(*score)); score->accepted_references = reference_matches; score->frame_identity_ambiguous = reference_matches > 1; return 0; }
int x4_probe_reference_close(X4ProbeReference *r) { (void)r; return reference_close_rc; }
#endif
static void reset_fixture(void)
{
    for (unsigned i = 1; i <= next_reservation; ++i) CHECK(!reservations[i].reserved);
    next_reservation = 0; memset(reservations, 0, sizeof(reservations));
    query_rc = create_rc = decode_rc = flush_rc = delete_rc = release_rc = join_rc = 0;
    query_calls = create_calls = decode_calls = flush_calls = delete_calls = release_calls = join_calls = 0;
    valid_output = false;
    output_height_override = output_pitch_override = 0; reference_matches = 1;
    output_picture_count = 1;
    reference_open_rc = reference_close_rc = 0; query_cpu_bytes = query_gpu_bytes = 16384; query_frame_extra = 0;
    reject_cacheable_output_allocate=false; memset(&seen_config,0,sizeof(seen_config));memset(&created_config,0,sizeof(created_config));
#if defined(X4_NATIVE_MOCK_STREAM_FAULTS)
    stream_query_fault=NULL;stream_create_fault=NULL;stream_create_success_null=false;
    memset(stream_query_configs,0,sizeof(stream_query_configs));memset(stream_create_configs,0,sizeof(stream_create_configs));
#endif
#if X4_VIDEO_PROBE
    for(unsigned i=0;i<X4_PROBE_STAGE_COUNT;++i)atomic_store(&observed_copy_stages[i],0);
#endif
}
static unsigned reserved_count(void)
{ unsigned n = 0; for (unsigned i = 1; i <= next_reservation; ++i) n += reservations[i].reserved; return n; }
static int start(X4LiveVideo *v, uint32_t *pixels, bool local, atomic_bool *cancel)
{
#if X4_VIDEO_PROBE
    if (local) return x4_live_video_probe_start(v, pixels, X4_LIVE_RGB_PIXELS, &x4_probe_cases[0], &hash_context, cancel);
#else
    (void)local; (void)cancel;
#endif
    return x4_live_video_start(v, pixels, X4_LIVE_RGB_PIXELS);
}
static void query_create_failures(uint32_t *pixels)
{
    atomic_bool cancel = false; X4LiveVideo v = {0}; reset_fixture(); query_rc = -101;
    CHECK(start(&v, pixels, true, &cancel) == -101); CHECK(query_calls == 1 && create_calls == 0);
    CHECK(v.state == NULL && reserved_count() == 0);
#if X4_VIDEO_PROBE
    CHECK(v.probe_status.query_result == -101);
    CHECK(v.probe_status.create_result == X4_PROBE_RC_NOT_RUN && v.probe_status.decode_result == X4_PROBE_RC_NOT_RUN);
#endif
    reset_fixture(); v = (X4LiveVideo){0}; create_rc = -202;
    CHECK(start(&v, pixels, true, &cancel) == -202); CHECK(query_calls == 1 && create_calls == 1);
    CHECK(v.state == NULL && reserved_count() == 0);
#if X4_VIDEO_PROBE
    CHECK(v.probe_status.query_result == 0 && v.probe_status.create_result == -202);
    CHECK(v.probe_status.decode_result == X4_PROBE_RC_NOT_RUN);
#if X4_VIDEO_CAPACITY_1080
    reset_fixture(); v = (X4LiveVideo){0}; query_cpu_bytes = query_gpu_bytes = 64u * 1024u * 1024u;
    CHECK(start(&v, pixels, true, &cancel) == -6);
    CHECK(query_calls == 1 && create_calls == 0);
    CHECK(v.probe_status.query_result == 0 && v.probe_status.create_result == X4_PROBE_RC_NOT_RUN);
    CHECK(v.state == NULL && reserved_count() == 0); /* 720p query succeeds; aggregate policy refuses */
#endif
    reset_fixture(); v = (X4LiveVideo){0}; reference_open_rc = X4_PROBE_CANCELLED;
    CHECK(start(&v, pixels, true, &cancel) == X4_PROBE_CANCELLED);
    CHECK(v.state == NULL && reserved_count() == 0 && query_calls == 0 && create_calls == 0);
    CHECK(v.probe_status.reference_result == X4_PROBE_CANCELLED);
    CHECK(v.probe_status.query_result == X4_PROBE_RC_NOT_RUN);
#endif
}
static void feed_convert_flush(uint32_t *pixels)
{
    atomic_bool cancel = false; X4LiveVideo v = {0}; reset_fixture();
    CHECK(start(&v, pixels, false, &cancel) == 0);
    CHECK(seen_config.profile == 66 && seen_config.maxLevel == 31);
    CHECK(seen_config.maxFrameWidth == 1280 && seen_config.maxFrameHeight == 720 && seen_config.maxDpbFrameCount == 4);
    CHECK(reserved_count() == 10); valid_output = true; output_picture_count = 0;
    ((VideoState *)v.state)->streaming_copy = false; ((VideoState *)v.state)->copy_check_done = true;
    const uint8_t au[] = {0x65, 0x80};
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 0) == 0);
    CHECK(v.decoded_frames == 1 && v.frames == 0);
    CHECK(x4_live_video_convert_pending(&v) == 0);
    CHECK(v.frames == 1 && v.width == 1280 && v.height == 720);
    CHECK(pixels[0] == 0xff000000 && pixels[1280u * 720u - 1] == 0xff000000);
    /* The ordinary path keeps its previous acceptance of this unverified
     * pictureCount field; only the experimental probe gates unknown layout. */
    CHECK(x4_live_video_feed(&v, au, X4_LIVE_AU_MAX + 1u, 0) < 0 && decode_calls == 1);
    decode_rc = -303; CHECK(x4_live_video_feed(&v, au, sizeof(au), 1) == -303);
    CHECK(v.state != NULL && reserved_count() == 10); CHECK(x4_live_video_stop(&v) == 0);
    CHECK(v.state == NULL && reserved_count() == 0);
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, false, &cancel) == 0);
    valid_output = true; output_pitch_override = 1281;
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 0) < 0);
    CHECK(v.frames == 0 && v.decoded_frames == 0); CHECK(x4_live_video_stop(&v) == 0);
#if X4_VIDEO_PROBE
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, true, &cancel) == 0);
    CHECK(v.probe_status.query_result == 0 && v.probe_status.create_result == 0);
    CHECK(v.probe_status.decode_result == X4_PROBE_RC_NOT_RUN);
    CHECK(x4_live_video_probe_flush(&v) == 0 && flush_calls == 1);
    flush_rc = -404; CHECK(x4_live_video_probe_flush(&v) == -404);
    CHECK(v.probe_status.flush_result == -404);
    CHECK(v.probe_status.decode_result == X4_PROBE_RC_NOT_RUN);
    CHECK(x4_live_video_stop(&v) == 0);
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, true, &cancel) == 0);
    atomic_store(&cancel, true); CHECK(x4_live_video_probe_flush(&v) == X4_PROBE_CANCELLED && flush_calls == 0);
    CHECK(v.probe_status.flush_result == X4_PROBE_RC_NOT_RUN);
    CHECK(x4_live_video_stop(&v) == 0);
    reset_fixture(); v = (X4LiveVideo){0}; atomic_store(&cancel, false);
    CHECK(start(&v, pixels, true, &cancel) == 0); valid_output = true;
    ((VideoState *)v.state)->streaming_copy = false; ((VideoState *)v.state)->copy_check_done = true;
    for (unsigned i = 0; i < 3; ++i) CHECK(x4_live_video_feed(&v, au, sizeof(au), i) == 0);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_VERIFIED);
    CHECK(v.probe_status.layout_checks == 3 && v.probe_status.layout_reference_matches == 3);
    CHECK(v.frames == 1 && v.decoded_frames == 3);
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 3) == 0 && v.frames == 1);
    CHECK(x4_live_video_convert_pending(&v) == 0 && v.frames == 2 && v.decoded_frames == 4);
    CHECK(x4_live_video_stop(&v) == 0);
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, true, &cancel) == 0);
    valid_output = true; reference_matches = 0;
    ((VideoState *)v.state)->streaming_copy = false; ((VideoState *)v.state)->copy_check_done = true;
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 0) < 0);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_FAILED && v.frames == 0);
    CHECK(x4_live_video_stop(&v) == 0);
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, true, &cancel) == 0);
    valid_output = true; output_picture_count = 0;
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 0) < 0);
    CHECK(v.probe_status.decode_result == 0 && v.probe_status.output_reject_reason == 5);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_INDETERMINATE && v.frames == 0);
    CHECK(x4_live_video_stop(&v) == 0);
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, true, &cancel) == 0);
    valid_output = true; output_picture_count = 0;
    CHECK(x4_live_video_probe_flush(&v) < 0);
    CHECK(v.probe_status.flush_result == 0 && v.probe_status.decode_result == X4_PROBE_RC_NOT_RUN);
    CHECK(v.probe_status.output_reject_reason == 5);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_INDETERMINATE && v.frames == 0);
    CHECK(x4_live_video_stop(&v) == 0);
#if X4_VIDEO_CAPACITY_1080
    reset_fixture(); v = (X4LiveVideo){0};
    CHECK(x4_live_video_probe_start(&v, pixels, X4_LIVE_RGB_PIXELS, &x4_probe_cases[2], &hash_context, &cancel) == 0);
    valid_output = true; output_height_override = 1080; reference_matches = 2;
    ((VideoState *)v.state)->streaming_copy = false; ((VideoState *)v.state)->copy_check_done = true;
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 0) < 0);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_INDETERMINATE);
    CHECK(v.probe_status.layout_checks == 2 && v.probe_status.layout_ambiguous == 1 && v.frames == 0);
    CHECK(x4_live_video_stop(&v) == 0);
#endif
#endif
}
#if X4_VIDEO_PROBE
static void memory_policy_and_selection(uint32_t *pixels)
{
    atomic_bool cancel=false; X4LiveVideo v={0};
    reset_fixture(); CHECK(start(&v,pixels,true,&cancel)==0);
    VideoState *state=v.state;
    for(unsigned i=0;i<RING;++i) CHECK(reservations[state->output[i].offset].type==3);
    CHECK(v.probe_status.output_memory_type==3 && v.probe_status.output_memory_protection==0x33);
    CHECK(v.probe_status.native_policy_budget==(X4_VIDEO_CAPACITY_1080 ? 128u*1024u*1024u : X4_VIDEO_LEGACY_NATIVE_CEILING));
    CHECK(!x4_live_video_stop(&v) && !reserved_count());

    reset_fixture(); v=(X4LiveVideo){0};
    CHECK(x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[0],
        &hash_context,&cancel,10,NULL)<0);
    CHECK(query_calls==0 && create_calls==0 && !reserved_count());

    reset_fixture(); v=(X4LiveVideo){0};
    CHECK(x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[0],
        &hash_context,&cancel,0,NULL)==0);
    state=v.state;
    for(unsigned i=0;i<RING;++i) CHECK(reservations[state->output[i].offset].type==0);
    CHECK(v.probe_status.output_memory_type==0 && v.probe_status.output_memory_protection==0x33);
    /* CPU/GPU decoder contexts and input allocations keep their old types. */
    CHECK(reservations[state->compute.offset].type==0);
    CHECK(reservations[state->cpu.offset].type==0 && reservations[state->gpu.offset].type==3);
    CHECK(reservations[state->shared.offset].type==0);
    CHECK(!x4_live_video_stop(&v) && !reserved_count());

    reset_fixture(); v=(X4LiveVideo){0}; reject_cacheable_output_allocate=true;
    CHECK(x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[0],
        &hash_context,&cancel,0,NULL)==-818);
    CHECK(!v.state && !reserved_count() && !create_calls && !decode_calls);
    CHECK(v.probe_status.create_result==X4_PROBE_RC_NOT_RUN);
    reset_fixture(); v=(X4LiveVideo){0}; create_rc=-819;
    CHECK(x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[0],
        &hash_context,&cancel,0,NULL)==-819);
    CHECK(!v.state && !reserved_count() && create_calls==1 && !decode_calls);
    CHECK(v.probe_status.create_result==-819);
    reset_fixture(); v=(X4LiveVideo){0};
    CHECK(x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[0],
        &hash_context,&cancel,0,NULL)==0);
    const uint8_t au[]={0x65,0x80}; decode_rc=-820;
    CHECK(x4_live_video_feed(&v,au,sizeof(au),0)==-820 && decode_calls==1);
    CHECK(v.probe_status.decode_result==-820 && v.frames==0);
    CHECK(!x4_live_video_stop(&v) && !reserved_count());

    /* The budget uses separately rounded reservations, not only raw sums.
     * These synthetic query sizes test the admission boundary, not console
     * memory requirements or decoder compatibility. */
    const size_t budget=128u*1024u*1024u;
    size_t frame=0;
    CHECK(!x4_video_size_align(1280u*720u*3u/2u,16384,&frame));
    const size_t other=2u*X4_LIVE_AU_MAX+2u*16384u+4u*frame;
    reset_fixture(); v=(X4LiveVideo){0};
    query_cpu_bytes=64u*1024u*1024u; query_gpu_bytes=budget-other-query_cpu_bytes;
    CHECK(query_gpu_bytes<64u*1024u*1024u && query_gpu_bytes%16384u==0);
    CHECK(start(&v,pixels,true,&cancel)==0);
    CHECK(v.probe_status.rounded_native_bytes==budget);
    CHECK(!x4_live_video_stop(&v) && !reserved_count());
    reset_fixture(); v=(X4LiveVideo){0};
    query_cpu_bytes=64u*1024u*1024u; query_gpu_bytes=budget-other-query_cpu_bytes+1u;
#if X4_VIDEO_CAPACITY_1080
    CHECK(start(&v,pixels,true,&cancel)==-6);
    CHECK(v.probe_status.rounded_native_bytes==budget+16384u);
    CHECK(query_calls==1 && !create_calls && !v.state && !reserved_count());
    /* Console planned total from f2edf was below the new bound. Shape is
     * 1920x1088 coded; the host query's component split is synthetic. */
    reset_fixture(); v=(X4LiveVideo){0};
    CHECK(!x4_video_size_align(1920u*1088u*3u/2u,16384,&frame));
    query_cpu_bytes=32u*1024u*1024u;
    query_gpu_bytes=106807296u-(2u*X4_LIVE_AU_MAX+2u*16384u+4u*frame)-query_cpu_bytes;
    CHECK(x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[2],
        &hash_context,&cancel,3,NULL)==0);
    CHECK(v.probe_status.rounded_native_bytes==106807296u);
    CHECK(v.probe_status.native_policy_budget==budget);
    CHECK(!x4_live_video_stop(&v) && !reserved_count());
#else
    CHECK(start(&v,pixels,true,&cancel)==0);
    CHECK(v.probe_status.rounded_native_bytes==budget+16384u);
    CHECK(v.probe_status.native_policy_budget==X4_VIDEO_LEGACY_NATIVE_CEILING);
    CHECK(!x4_live_video_stop(&v) && !reserved_count());
    reset_fixture(); v=(X4LiveVideo){0};
    CHECK(x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[2],
        &hash_context,&cancel,3,NULL)<0);
    CHECK(!query_calls && !create_calls && !reserved_count());
#endif
}

static void copy_timer_contract(uint32_t *pixels)
{
    atomic_bool cancel=false; X4LiveVideo v={0}; reset_fixture();
    /* Only our record_duration double observes this marker. No real metrics
     * object is read, and synthetic clock spans are not PS4 performance. */
    X4ProbeMetrics *marker=(X4ProbeMetrics *)(uintptr_t)1;
    CHECK(!x4_live_video_probe_start_memory(&v,pixels,X4_LIVE_RGB_PIXELS,&x4_probe_cases[0],
        &hash_context,&cancel,3,marker));
    VideoState *state=v.state; CHECK(state->parallel_enabled);
    valid_output=true; state->streaming_copy=false; state->copy_check_done=true;
    const uint8_t au[]={0x65,0x80};
    for(unsigned i=0;i<3;++i) CHECK(!x4_live_video_feed(&v,au,sizeof(au),i));
    CHECK(v.probe_status.layout_verdict==X4_PROBE_LAYOUT_VERIFIED);
    /* Layout/reference verification may copy a picture again before normal
     * conversion. Match real completed copy calls, not decoded-picture count. */
    CHECK(v.copy_calls>=3 && v.copy_parallel_calls==v.copy_calls);
    CHECK(observed_copy_stages[X4_PROBE_STAGE_COPY_OWNER_LOOP]==v.copy_calls);
    CHECK(observed_copy_stages[X4_PROBE_STAGE_COPY_READERS_MAX_LOOP]==v.copy_calls);
    CHECK(observed_copy_stages[X4_PROBE_STAGE_COPY_COMPLETION_WAIT]==v.copy_calls);
    for(unsigned i=0;i<3;++i) CHECK(observed_copy_stages[X4_PROBE_STAGE_COPY_HELPER0_LOOP+i]==v.copy_calls);
    printf("HOST copy stages recorded once per actual copy: calls=%llu native_outputs=%llu\n",
        (unsigned long long)v.copy_calls,(unsigned long long)v.decoded_frames);
    CHECK(!x4_live_video_stop(&v) && !reserved_count());
}

static void metadata_change_and_reference_close(uint32_t *pixels)
{
    const uint8_t au[] = {0x65, 0x80}; atomic_bool cancel = false;
    X4LiveVideo v = {0}; reset_fixture(); query_frame_extra = 65536;
    CHECK(start(&v, pixels, true, &cancel) == 0); valid_output = true;
    ((VideoState *)v.state)->streaming_copy = false; ((VideoState *)v.state)->copy_check_done = true;
    for (unsigned i = 0; i < 3; ++i) CHECK(x4_live_video_feed(&v, au, sizeof(au), i) == 0);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_VERIFIED && v.frames == 1);
    /* The larger queried buffer makes the changed even pitch remain within
     * bounds. Established metadata must still invalidate the mapping before
     * any copy/conversion, rather than treating old arithmetic as new proof. */
    uint64_t copied = v.copy_calls; output_pitch_override = 1296;
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 3) == 0);
    CHECK(x4_live_video_convert_pending(&v) < 0);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_INDETERMINATE);
    CHECK(v.frames == 1 && v.copy_calls == copied);
    CHECK(x4_live_video_stop(&v) == 0 && reserved_count() == 0);
#if X4_VIDEO_CAPACITY_1080
    reset_fixture(); v = (X4LiveVideo){0};
    CHECK(x4_live_video_probe_start(&v, pixels, X4_LIVE_RGB_PIXELS, &x4_probe_cases[2], &hash_context, &cancel) == 0);
    valid_output = true;
    ((VideoState *)v.state)->streaming_copy = false; ((VideoState *)v.state)->copy_check_done = true;
    for (unsigned i = 0; i < 3; ++i) CHECK(x4_live_video_feed(&v, au, sizeof(au), i) == 0);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_VERIFIED && v.frames == 1);
    copied = v.copy_calls; output_height_override = 1080; /* native metadata 1088 -> 1080 */
    CHECK(x4_live_video_feed(&v, au, sizeof(au), 3) == 0);
    CHECK(x4_live_video_convert_pending(&v) < 0);
    CHECK(v.probe_status.layout_verdict == X4_PROBE_LAYOUT_INDETERMINATE);
    CHECK(v.frames == 1 && v.copy_calls == copied);
    CHECK(x4_live_video_stop(&v) == 0 && reserved_count() == 0);
#endif
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, true, &cancel) == 0);
    reference_close_rc = -707;
    CHECK(x4_live_video_stop(&v) == 0);
    CHECK(v.state == NULL && reserved_count() == 0 && v.probe_status.allocated_native_bytes == 0);
    CHECK(v.probe_status.close_result == 0 && v.probe_status.reference_close_result == -707);
    CHECK(v.probe_status.delete_decoder_result == 0 && v.probe_status.release_queue_result == 0);
    v.probe_status.pipeline_result = 0; v.probe_status.source_eof = true;
    CHECK(x4_probe_end_reason(&v.probe_status) == X4_PROBE_END_ERROR);
}
static void helper_stack_contract(uint32_t *pixels)
{
    atomic_bool cancel=false; X4LiveVideo v={0}; reset_fixture();
    x4_probe_stack_test_fail(2,22);
    CHECK(start(&v,pixels,true,&cancel)==-22);
    CHECK(decode_calls==0&&v.state==NULL&&reserved_count()==0);
    CHECK(v.probe_stacks[0].set_size_rc==22&&!v.probe_stacks[0].started);
    CHECK(strcmp(v.stage,"PILA LECTOR SONDA")==0);
    x4_probe_stack_test_fail(0,0);reset_fixture();v=(X4LiveVideo){0};
    CHECK(start(&v,pixels,true,&cancel)==0);CHECK(!x4_live_video_stop(&v));
    for(unsigned i=0;i<3;++i) {
        CHECK(v.probe_stacks[i].configured_bytes==262144&&v.probe_stacks[i].reported_bytes==262144);
        CHECK(v.probe_stacks[i].started&&v.probe_stacks[i].finished&&v.probe_stacks[i].join_rc==0);
    }
}
static void end_reason_contract(void)
{
    CHECK(x4_probe_end_reason(NULL) == X4_PROBE_END_SETUP_FAILURE);
    X4ProbeStatus s = x4_probe_status_pending();
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_ERROR);
    s.pipeline_result = 0; s.source_eof = true; s.close_result = 0;
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_CLEAN_EOF);
    s.pipeline_result = X4_PROBE_CANCELLED;
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_CANCEL);
    s.close_result = -1;
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_ERROR);
    s.close_result = 0; s.reference_close_result = -2;
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_ERROR);
    s.pipeline_result = 0;
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_ERROR);
    s.reference_close_result = 0; s.pipeline_result = -3;
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_ERROR);
    s.pipeline_result = 0; s.source_eof = false;
    CHECK(x4_probe_end_reason(&s) == X4_PROBE_END_ERROR);
}
#endif
static void retention(uint32_t *pixels)
{
    atomic_bool cancel = false; X4LiveVideo v = {0}; reset_fixture(); CHECK(start(&v, pixels, false, &cancel) == 0);
    unsigned live = reserved_count(); delete_rc = -505;
    CHECK(x4_live_video_stop(&v) == -505); CHECK(v.state != NULL && reserved_count() == live);
    CHECK(release_calls == 0 && delete_calls == 1);
    /* Fixture-only recovery: production media latches teardown failure and
     * must not retry native deletion. Reset the mock solely to release host memory. */
    delete_rc = 0; CHECK(x4_live_video_stop(&v) == 0); CHECK(reserved_count() == 0);
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, false, &cancel) == 0);
    VideoState *s = v.state; s->helpers[0].running = true; join_rc = 17;
    CHECK(x4_live_video_stop(&v) == -17); CHECK(delete_calls == 0 && release_calls == 0);
    CHECK(v.state == s && reserved_count() == live);
    CHECK(x4_live_video_stop(&v) == -17 && join_calls == 1); /* existing join-error latch */
    /* No real worker was created in this mock; remove the simulated lifetime
     * only for fixture cleanup. This does not test native thread cancellation. */
    s->helpers[0].running = false; s->copy_join_error = 0; join_rc = 0;
    CHECK(x4_live_video_stop(&v) == 0 && reserved_count() == 0);
    reset_fixture(); v = (X4LiveVideo){0}; CHECK(start(&v, pixels, false, &cancel) == 0);
    release_rc = -606; CHECK(x4_live_video_stop(&v) == -606);
    CHECK(v.state != NULL && reserved_count() == live);
    CHECK(((VideoState *)v.state)->output[0].p == NULL && ((VideoState *)v.state)->output[0].allocated);
    release_rc = 0; CHECK(x4_live_video_stop(&v) == 0 && reserved_count() == 0);
}
int main(void)
{
    uint32_t *pixels = calloc(X4_LIVE_RGB_PIXELS, sizeof(*pixels)); CHECK(pixels);
    query_create_failures(pixels); feed_convert_flush(pixels); retention(pixels);
#if X4_VIDEO_PROBE
    metadata_change_and_reference_close(pixels); end_reason_contract(); helper_stack_contract(pixels);
    memory_policy_and_selection(pixels);
    copy_timer_contract(pixels);
#endif
    reset_fixture(); free(pixels);
    printf("PASS HOST MOCK native calls probe=%u capacity=%u checks=%u; no PS4 compatibility/performance/concurrency claim\n",
        (unsigned)X4_VIDEO_PROBE, (unsigned)X4_VIDEO_CAPACITY_1080, checks); return 0;
}
