/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_pipeline.h"
#if X4_VIDEO_PROBE
#include "video_pipeline.h"
#include "live_h264.h"
#include "probe_files.h"
#include "probe_report_file.h"
#include "../core/build_identity.h"
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>
#include <orbis/Pad.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

enum { PIPE_TIMEOUT_US=10000000, SCANOUT_BYTES=0x800000,
    PIPE_NATIVE_BUDGET=128*1024*1024 };
/* Only the main probe controller submits flips. Keep IDs unique across cases
 * in this process, outside the menu's small counter namespace. The SDK does
 * not document whether GetFlipStatus history resets when a handle reopens. */
static uint64_t next_pipeline_flip=UINT64_C(1)<<62;
typedef struct Pipeline Pipeline;
typedef struct {
    Pipeline *owner; unsigned index;
    OrbisPthread id; X4ProbeThread thread;
    atomic_bool stop, finished;
    atomic_uint_fast64_t submitted, completed;
    X4ProbeNativeOutput image; uint32_t *target;
    int result; uint64_t duration;
} Converter;
typedef struct {
    int handle, allocate_rc, map_rc, register_rc, rate_rc, close_rc, unmap_rc, release_rc;
    void *memory; off_t offset; bool allocated;
    uint32_t *pixels[3]; X4VideoPipelineDisplay ledger;
    size_t bytes;
} Scanout;
struct Pipeline {
    const X4ProbeCase *sample; X4ProbeMode mode; unsigned converters;
    X4ProbePipelineRunResult result;
    X4LiveVideo video; X4ProbeSource source; X4ProbeHashContext hash;
    /* Main is the sole per-AU file reader after source_ready. Two bounded
     * slots keep file I/O/pacing out of the dedicated native decode worker. */
    uint8_t *au[2]; X4ProbeAU input[2]; uint32_t *reference_pixels;
    atomic_bool source_ready, input_eof;
    atomic_int input_error;
    atomic_uint_fast64_t input_submitted, input_consumed;
    X4VideoPipelineRing ring; X4ProbeNativeOutput images[4], last_image;
    atomic_flag gate;
    atomic_bool cancel, produced_done, cleanup_allowed, decoder_finished;
    atomic_uint_fast64_t outputs, conversions, measurement_start;
    OrbisPthread decoder; X4ProbeThread decoder_thread;
    Converter workers[3]; Scanout display;
    X4ProbeMetrics *metrics;
    uint64_t reference_end_us, input_begin_us, decode_finished_us;
    uint64_t measured_decode_outputs, decode_observed, convert_observed, presented;
    uint64_t no_output_calls, last_present_us, last_decode_output_us;
    uint64_t scanout_observations, scanout_occupied_samples[4], scanout_inflight_samples[4];
    int error, source_close_result;
    bool display_error, native_retained;
};
static uint64_t clock_us(void) { return sceKernelGetProcessTime(); }
/* Non-network workers only. Lock covers four small slots, never a native call,
 * conversion, file operation or metric recorder. Yield rather than hot spin. */
static void ring_lock(Pipeline *p)
{ while(atomic_flag_test_and_set_explicit(&p->gate,memory_order_acquire)) sceKernelUsleep(50); }
static void ring_unlock(Pipeline *p)
{ atomic_flag_clear_explicit(&p->gate,memory_order_release); }
static void duration(Pipeline *p,X4ProbeStage stage,uint64_t end,uint64_t us)
{ x4_probe_metrics_record_duration(p->metrics,stage,end,us); }
static void note(Pipeline *p,X4ProbeCounter c,uint64_t n)
{ x4_probe_metrics_note_at(p->metrics,clock_us(),c,n); }
static bool cancelled(Pipeline *p)
{ return atomic_load_explicit(&p->cancel,memory_order_acquire); }
static void *probe_pipeline_convert_worker(void *argument)
{
    Converter *w=argument; uint64_t seen=0;
    for(;;) {
        uint64_t work=atomic_load_explicit(&w->submitted,memory_order_acquire);
        if(work!=seen) {
            uint64_t begin=clock_us();
            w->result=x4_video_pipeline_convert_stripe(&w->image.layout,w->image.pixels,
                w->image.owned_bytes,w->target,SCANOUT_BYTES,1920,w->owner->converters,w->index);
            uint64_t end=clock_us(); w->duration=end-begin;
            duration(w->owner,(X4ProbeStage)(X4_PROBE_STAGE_STRIPE0+w->index),end,w->duration);
            seen=work; atomic_store_explicit(&w->completed,work,memory_order_release);
        } else if(atomic_load_explicit(&w->stop,memory_order_acquire)) break;
        else sceKernelUsleep(100);
    }
    atomic_store_explicit(&w->finished,true,memory_order_release); return NULL;
}
static int probe_pipeline_source_open(Pipeline *p)
{
    int rc=x4_probe_source_open_fully_verified(&p->source,p->sample->clip_path,p->sample->index_path,
        p->sample->clip_sha256,p->sample->index_sha256,x4_probe_hash_file,&p->hash,&p->cancel);
    if(rc) return rc;
    const X4ProbeSourceInfo *i=&p->source.info; const X4ProbeCase *c=p->sample;
    if(i->records!=c->au_count || i->duration_us!=c->duration_us ||
       i->visible_width!=c->visible_width || i->visible_height!=c->visible_height ||
       i->coded_width!=c->coded_width || i->coded_height!=c->coded_height ||
       i->fps_num!=c->fps_num || i->fps_den!=c->fps_den) return X4_PROBE_INDEX;
    return 0;
}
static int acquire_native(Pipeline *p,X4VideoPipelineLease *lease)
{
    uint64_t begin=clock_us();
    for(;;) {
        uint64_t discarded=0; ring_lock(p);
        int rc=x4_video_pipeline_acquire_decode(&p->ring,lease,&discarded);
        ring_unlock(p);
        if(discarded) note(p,X4_PROBE_COUNT_OUTPUT_SUPERSEDE,1);
        if(rc!=X4_PIPELINE_WAIT) { uint64_t end=clock_us();duration(p,X4_PROBE_STAGE_FREE_OUTPUT_WAIT,end,end-begin);return rc; }
        uint64_t now=clock_us();
        if(cancelled(p) || now-begin>PIPE_TIMEOUT_US) {
            duration(p,X4_PROBE_STAGE_FREE_OUTPUT_WAIT,now,now-begin);
            return cancelled(p)?X4_PROBE_CANCELLED:X4_PIPELINE_EXHAUSTED;
        }
        sceKernelUsleep(100);
    }
}
static int accept_native_result(Pipeline *p,X4VideoPipelineLease offered,X4ProbeNativeOutput *out)
{
    int rc=0; ring_lock(p);
    /* A returned native pointer identifies its prior accepted lease. It does
     * not identify the AU passed to this call, nor necessarily offered.slot. */
    if(out->valid) {
        X4VideoPipelineLease returned;
        rc=x4_video_pipeline_decoding_lease(&p->ring,out->slot,&returned);
        if(!rc) {
            p->images[out->slot]=*out;
            rc=x4_video_pipeline_publish_ready(&p->ring,returned,out->output_id,out->decode_end_us);
        }
    }
    if(!rc && !out->accepted && (!out->valid || out->slot!=offered.slot))
        rc=x4_video_pipeline_abort_decode(&p->ring,offered);
    /* Diagnostic first three or decode-only release after validation. There
     * is a single ready item in decode-only, and no unvalidated item is ever
     * handed to conversion workers. */
    if(!rc && out->valid && (!p->converters || !out->layout_verified)) {
        X4VideoPipelineLease discard;
        rc=x4_video_pipeline_take_ready(&p->ring,&discard,NULL);
        if(!rc) rc=x4_video_pipeline_release_conversion(&p->ring,discard);
    }
    ring_unlock(p); return rc;
}
static void *probe_pipeline_decode_worker(void *argument)
{
    Pipeline *p=argument; int rc=probe_pipeline_source_open(p), source_rc=rc;
    p->video.probe_status.source_result=rc;
    if(!rc) rc=x4_live_video_probe_raw_start(&p->video,p->reference_pixels,X4_LIVE_RGB_PIXELS,
        p->sample,&p->hash,&p->cancel);
    /* start initializes the public status. Preserve the verified source fact. */
    p->video.probe_status.source_result=source_rc;
    if(!rc) { p->input_begin_us=clock_us(); rc=x4_probe_source_begin(&p->source,p->input_begin_us,p->mode); }
    if(!rc)atomic_store_explicit(&p->source_ready,true,memory_order_release);
    bool draining=false; unsigned drain_calls=0;
    uint64_t last_input_us=0;
    while(!rc && !cancelled(p)) {
        X4ProbeAU au={0};unsigned input_slot=0;uint64_t consumed=0;
        if(!draining) {
            consumed=atomic_load_explicit(&p->input_consumed,memory_order_relaxed);
            uint64_t submitted=atomic_load_explicit(&p->input_submitted,memory_order_acquire);
            if(consumed==submitted) {
                if(!atomic_load_explicit(&p->input_eof,memory_order_acquire)) {sceKernelUsleep(100);continue;}
                /* EOF acquisition may follow a newer publication than our
                 * first submitted load. Drain that last AU before Flush. */
                if(atomic_load_explicit(&p->input_submitted,memory_order_acquire)!=consumed)continue;
                rc=atomic_load_explicit(&p->input_error,memory_order_relaxed);if(rc)break;
                draining=true;p->video.probe_status.source_eof=true;note(p,X4_PROBE_COUNT_EOF,1);
            } else {input_slot=(unsigned)(consumed%2);au=p->input[input_slot];}
        }
        if(draining && ++drain_calls>32) { rc=X4_PROBE_STATE; break; }
        X4VideoPipelineLease lease;
        rc=acquire_native(p,&lease); if(rc) break;
        if(!draining) {
            uint64_t now=clock_us();
            if(last_input_us) duration(p,X4_PROBE_STAGE_INPUT_INTERVAL,now,now-last_input_us);
            last_input_us=now; note(p,X4_PROBE_COUNT_INPUT_AU,1);
            uint64_t wait=0,late=0;
            if(x4_probe_schedule(p->input_begin_us,au.pts_us,now,p->mode,&wait,&late)) {rc=X4_PROBE_CLOCK;break;}
            duration(p,X4_PROBE_STAGE_INPUT_LATENESS,now,late);
            ++p->video.probe_status.input_aus; p->video.probe_status.last_input_pts_us=au.pts_us;
            if(late) ++p->video.probe_status.late_inputs;
            if(late>p->video.probe_status.max_input_late_us) p->video.probe_status.max_input_late_us=late;
        }
        X4ProbeNativeOutput output={0};
        rc=x4_live_video_probe_raw_decode(&p->video,lease.slot,p->au[input_slot],(size_t)au.bytes,au.pts_us,draining,&output);
        if(!draining)atomic_store_explicit(&p->input_consumed,consumed+1,memory_order_release);
        if(output.decode_end_us)duration(p,X4_PROBE_STAGE_DECODE,output.decode_end_us,output.decode_us);
        if(rc) break; /* ownership released only after successful Delete */
        rc=accept_native_result(p,lease,&output); if(rc) break;
        if(output.valid) {
            p->last_image=output; ++p->decode_observed;
            uint64_t boundary=atomic_load_explicit(&p->measurement_start,memory_order_acquire);
            if(boundary && output.decode_end_us>=boundary) ++p->measured_decode_outputs;
            p->last_decode_output_us=output.decode_end_us;
            note(p,X4_PROBE_COUNT_NATIVE_OUTPUT,1);
            atomic_store_explicit(&p->outputs,p->decode_observed,memory_order_release);
            if(!p->converters && p->decode_observed>=60 && output.layout_verified && !boundary) {
                uint64_t start=clock_us();
                if(x4_probe_metrics_begin_measurement(p->metrics,start))
                    atomic_store_explicit(&p->measurement_start,start,memory_order_release);
            }
            if(output.layout_verified && p->reference_pixels) {
                p->reference_end_us=clock_us(); free(p->reference_pixels); p->reference_pixels=NULL; p->video.pixels=NULL;
            }
        } else ++p->no_output_calls;
        if(draining && !output.valid) break;
    }
    if(cancelled(p) && !rc) rc=X4_PROBE_CANCELLED;
    p->video.probe_status.pipeline_result=rc; p->video.probe_status.cancel_requested=cancelled(p);
    p->decode_finished_us=clock_us();
    atomic_store_explicit(&p->produced_done,true,memory_order_release);
    /* The main owner signals only after every conversion has stopped reading. */
    while(!atomic_load_explicit(&p->cleanup_allowed,memory_order_acquire)) sceKernelUsleep(100);
    int close=x4_live_video_stop(&p->video);
    p->source_close_result=x4_probe_source_close(&p->source);
    if(!close && p->source_close_result) close=p->source_close_result;
    p->video.probe_status.close_result=close;
    p->native_retained=p->video.state!=NULL;
    if(!p->native_retained) {
        ring_lock(p); (void)x4_video_pipeline_cancel_ready(&p->ring);
        for(unsigned i=0;i<4;++i) {
            X4VideoPipelineLease old;
            if(!x4_video_pipeline_decoding_lease(&p->ring,i,&old)) (void)x4_video_pipeline_abort_decode(&p->ring,old);
        }
        ring_unlock(p);
    }
    p->video.probe_status.finished=true;
    atomic_store_explicit(&p->decoder_finished,true,memory_order_release); return NULL;
}
static void scanout_init(Scanout *s,uint64_t epoch)
{
    memset(s,0,sizeof(*s)); s->handle=-1;
    s->allocate_rc=s->map_rc=s->register_rc=s->rate_rc=s->close_rc=s->unmap_rc=s->release_rc=X4_PROBE_RC_NOT_RUN;
    x4_video_pipeline_display_init(&s->ledger,epoch);
}
static int scanout_open(Scanout *s)
{
    s->handle=sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,ORBIS_VIDEO_OUT_BUS_MAIN,0,NULL);
    if(s->handle<0) return s->handle;
    s->bytes=3u*SCANOUT_BYTES;
    int rc=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),s->bytes,0x200000,3,&s->offset);
    s->allocate_rc=rc; if(rc)return rc; s->allocated=true;
    rc=sceKernelMapDirectMemory(&s->memory,s->bytes,0x33,0,s->offset,0x200000);s->map_rc=rc;if(rc)return rc;
    memset(s->memory,0,s->bytes);
    for(unsigned i=0;i<3;++i)s->pixels[i]=(uint32_t*)((uint8_t*)s->memory+i*SCANOUT_BYTES);
    OrbisVideoOutBufferAttribute attribute={0};
    sceVideoOutSetBufferAttribute(&attribute,ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
        ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,1920,1080,1920);
    rc=sceVideoOutRegisterBuffers(s->handle,0,(void *const*)s->pixels,3,&attribute);s->register_rc=rc;if(rc)return rc;
    rc=sceVideoOutSetFlipRate(s->handle,ORBIS_VIDEO_OUT_FLIP_60HZ);s->rate_rc=rc;return rc;
}
static int scanout_close(Scanout *s)
{
    if(s->handle>=0) {s->close_rc=sceVideoOutClose(s->handle);if(s->close_rc)return s->close_rc;s->handle=-1;}
    int ledger_rc=x4_video_pipeline_display_closed(&s->ledger);if(ledger_rc)return ledger_rc;
    if(s->memory) {s->unmap_rc=sceKernelMunmap(s->memory,s->bytes);if(s->unmap_rc)return s->unmap_rc;s->memory=NULL;}
    if(s->allocated) {s->release_rc=sceKernelReleaseDirectMemory(s->offset,s->bytes);if(s->release_rc)return s->release_rc;s->allocated=false;}
    return 0;
}
static int display_tick(Pipeline *p)
{
    Scanout *d=&p->display;
    if(d->ledger.pending>=0) {
        OrbisVideoOutFlipStatus status={0}; int rc=sceVideoOutGetFlipStatus(d->handle,&status);if(rc)return rc;
        uint64_t now=clock_us(); X4VideoPipelineLease shown;
        int observed=x4_video_pipeline_observe_flip(&d->ledger,(uint64_t)status.flipArg,now,&shown);
        if(!observed) {
            X4VideoPipelineScanout *s=&d->ledger.slots[shown.slot];
            duration(p,X4_PROBE_STAGE_PIPELINE_FLIP_OBSERVATION,now,now-s->submitted_us);
            duration(p,X4_PROBE_STAGE_DECODE_TO_FLIP,now,now-s->decode_end_us);
            if(p->last_present_us) duration(p,X4_PROBE_STAGE_NEW_PRESENT_INTERVAL,now,now-p->last_present_us);
            p->last_present_us=now; ++p->presented; note(p,X4_PROBE_COUNT_NEW_PRESENT,1);
        } else if(now-d->ledger.slots[d->ledger.pending].submitted_us>PIPE_TIMEOUT_US) return X4_PIPELINE_STATE;
    }
    X4VideoPipelineLease ready;
    int found=x4_video_pipeline_ready_scanout(&d->ledger,&ready);
    if(found==X4_PIPELINE_WAIT)return 0;
    if(found)return found;
    if(next_pipeline_flip>=(uint64_t)INT64_MAX) return X4_PIPELINE_EXHAUSTED;
    uint64_t begin=clock_us(), arg=next_pipeline_flip++;
    if(begin<d->ledger.slots[ready.slot].converted_us)return X4_PROBE_CLOCK;
    int rc=sceVideoOutSubmitFlip(d->handle,ready.slot,ORBIS_VIDEO_OUT_FLIP_VSYNC,(int64_t)arg);
    uint64_t end=clock_us();duration(p,X4_PROBE_STAGE_FLIP_CALL,end,end-begin);
    if(rc)return rc;
    /* Ends at the start of a successful local SubmitFlip call, not its return
     * or hardware display. flip_call and flip observation remain separate. */
    duration(p,X4_PROBE_STAGE_CONVERT_TO_SUBMIT,begin,begin-d->ledger.slots[ready.slot].converted_us);
    rc=x4_video_pipeline_mark_submitted(&d->ledger,ready,arg,end);if(rc)return rc;
    return sceGnmSubmitDone();
}
static int observe_scanout_occupancy(Pipeline *p)
{
    if(!p->converters)return 0;
    X4VideoPipelineDisplayOccupancy o;
    int rc=x4_video_pipeline_display_occupancy(&p->display.ledger,&o);if(rc)return rc;
    if(o.in_flight>1 || o.occupied>2)return X4_PIPELINE_STATE;
    ++p->scanout_observations;++p->scanout_occupied_samples[o.occupied];
    ++p->scanout_inflight_samples[o.in_flight];return 0;
}
static bool display_drained(const Scanout *d)
{
    if(d->ledger.pending>=0)return false;
    for(unsigned i=0;i<3;++i) if(d->ledger.slots[i].state==X4_SCANOUT_WRITING || d->ledger.slots[i].state==X4_SCANOUT_READY)return false;
    return true;
}
static void observe_measurement(Pipeline *p)
{
    if(!p->converters)return; /* decode-only boundary belongs to the producer */
    if(atomic_load(&p->measurement_start))return;
    uint64_t count=atomic_load_explicit(p->converters?&p->conversions:&p->outputs,memory_order_acquire);
    if(count>=60) {
        uint64_t now=clock_us();
        if(x4_probe_metrics_begin_measurement(p->metrics,now))atomic_store_explicit(&p->measurement_start,now,memory_order_release);
    }
}
static void control(Pipeline *p,X4Controller *pad,unsigned tick)
{
    x4_controller_read(pad,tick);
    if(pad->pressed&ORBIS_PAD_BUTTON_OPTIONS) {p->result.exit_requested=true;atomic_store(&p->cancel,true);}
    if(pad->pressed&ORBIS_PAD_BUTTON_CIRCLE)atomic_store(&p->cancel,true);
}
static void input_tick(Pipeline *p)
{
    if(cancelled(p) || !atomic_load_explicit(&p->source_ready,memory_order_acquire) ||
        atomic_load_explicit(&p->input_eof,memory_order_relaxed) ||
        atomic_load_explicit(&p->produced_done,memory_order_acquire)) return;
    uint64_t submitted=atomic_load_explicit(&p->input_submitted,memory_order_relaxed);
    uint64_t consumed=atomic_load_explicit(&p->input_consumed,memory_order_acquire);
    if(submitted-consumed>=2)return;
    unsigned slot=(unsigned)(submitted%2);
    int rc=x4_probe_source_next(&p->source,clock_us(),p->au[slot],X4_PROBE_AU_MAX,&p->input[slot]);
    if(!rc)atomic_store_explicit(&p->input_submitted,submitted+1,memory_order_release);
    else if(rc!=X4_PROBE_WAIT) {
        atomic_store_explicit(&p->input_error,rc==X4_PROBE_EOF?0:rc,memory_order_relaxed);
        atomic_store_explicit(&p->input_eof,true,memory_order_release);
    }
}
static int export_failure(const char *operation, const char *path, const char *key,
    uint64_t sequence, int result, int saved_errno)
{
    if (path[0])
        printf("XCloud4 pipeline: export_failed operation=%s path=%s result=%d errno=%d\n",
            operation, path, result, saved_errno);
    else
        printf("XCloud4 pipeline: export_failed operation=%s path_attempt=%s/xcloud4-probe-%.12s%s-%s-%llu.json result=%d errno=%d\n",
            operation, X4_PROBE_REPORT_ROOT, X4_SOURCE_ID,
            X4_NACK_ID X4_GRACE_ID X4_RUMBLE_ID X4_VIDEO_CAPACITY_ID X4_VIDEO_PROBE_ID X4_INGRESS_ID,
            key, (unsigned long long)sequence, result, saved_errno);
    errno = saved_errno;
    return result;
}
static int export_pipeline(Pipeline *p)
{
    char path[X4_PROBE_FILE_PATH_MAX] = {0}, key[128];
    snprintf(key,sizeof(key),"%s-%s-p%u-metrics",p->sample->id,p->mode==X4_PROBE_PACED?"paced":"maximum-speed",p->converters);
    errno = 0;
    int path_rc=x4_probe_files_report_path(path,sizeof(path),key,p->result.started_us), path_errno=errno;
    if(path_rc)return export_failure("metrics_path",path,key,p->result.started_us,path_rc,path_errno);
    errno = 0;
    int metrics_rc=x4_probe_metrics_dump_file(p->metrics,path), metrics_errno=errno;
    if(metrics_rc)return export_failure("metrics_write_finish",path,key,p->result.started_us,metrics_rc,metrics_errno);
    snprintf(key,sizeof(key),"%s-%s-p%u-pipeline",p->sample->id,p->mode==X4_PROBE_PACED?"paced":"maximum-speed",p->converters);
    path[0]=0; errno=0;
    path_rc=x4_probe_files_report_path(path,sizeof(path),key,p->result.started_us); path_errno=errno;
    if(path_rc)return export_failure("pipeline_path",path,key,p->result.started_us,path_rc,path_errno);
    errno=0;
    FILE *f=x4_probe_report_create(path); int create_errno=errno;
    if(!f)return export_failure("pipeline_create",path,key,p->result.started_us,-1,create_errno);
    errno=0;
    const X4ProbeStatus *s=&p->video.probe_status; const X4ProbeNativeOutput *o=&p->last_image;
    fprintf(f,"{\"schema\":\"X4PROBE_PIPELINE3\",\"ready_policy\":\"latest\",\"scanout_policy\":\"one_frame_in_flight\",\"source_id\":\"%s\",\"build_id\":\"%s\",\"case_id\":\"%s\",\"converters\":%u,\"mode\":\"%s\",\"decode_only\":%s,\"decode_pipeline_depth\":1,\"output_memory_type\":0,\"output_buffers\":4,\"videoout_buffers\":%u,\"input_output_PTS_association\":false,\"native_result_is_not_streaming_support\":true,",
        X4_SOURCE_ID,X4_BUILD_ID,p->sample->id,p->converters,p->mode==X4_PROBE_PACED?"paced":"maximum-speed",p->converters?"false":"true",p->converters?3:0);
    char hashes[3][65];const uint8_t *hashes_in[]={p->sample->clip_sha256,p->sample->index_sha256,p->sample->reference_sha256};
    for(unsigned h=0;h<3;++h) {for(unsigned i=0;i<32;++i)snprintf(hashes[h]+2*i,3,"%02x",hashes_in[h][i]);hashes[h][64]=0;}
    fprintf(f,"\"sample\":{\"clip_sha256\":\"%s\",\"index_sha256\":\"%s\",\"reference_sha256\":\"%s\",\"fps_num\":%u,\"fps_den\":%u,\"AU_count\":%llu,\"duration_us\":%llu,\"coded_width\":%u,\"coded_height\":%u,\"profile_idc\":%u,\"level_idc\":%u},\"late_inputs\":%llu,\"maximum_input_late_us\":%llu,",
        hashes[0],hashes[1],hashes[2],p->sample->fps_num,p->sample->fps_den,(unsigned long long)p->sample->au_count,(unsigned long long)p->sample->duration_us,p->sample->coded_width,p->sample->coded_height,p->sample->profile_idc,p->sample->level_idc,(unsigned long long)s->late_inputs,(unsigned long long)s->max_input_late_us);
    fprintf(f,"\"started_us\":%llu,\"measurement_start_us\":%llu,\"ended_us\":%llu,\"warmup_count\":60,\"warmup_basis\":\"%s\",\"input_begin_us\":%llu,\"native_producer_done_us\":%llu,\"input_AU\":%llu,\"native_outputs\":%llu,\"measured_decode_outputs\":%llu,\"RGB_publications\":%llu,\"new_presentations\":%llu,\"no_picture_calls\":%llu,",
        (unsigned long long)p->result.started_us,(unsigned long long)atomic_load(&p->measurement_start),(unsigned long long)p->result.ended_us,p->converters?"RGB_publications":"native_outputs",
        (unsigned long long)p->input_begin_us,(unsigned long long)p->decode_finished_us,(unsigned long long)s->input_aus,(unsigned long long)s->native_outputs,
        (unsigned long long)p->measured_decode_outputs,(unsigned long long)p->convert_observed,(unsigned long long)p->presented,(unsigned long long)p->no_output_calls);
    fprintf(f,"\"source_result\":%d,\"reference_result\":%d,\"query_result\":%d,\"compute_query_result\":%d,\"allocate_queue_result\":%d,\"create_result\":%d,\"decode_result\":%d,\"flush_result\":%d,\"delete_decoder_result\":%d,\"release_queue_result\":%d,\"reference_close_result\":%d,\"source_close_result\":%d,\"close_result\":%d,\"pipeline_result\":%d,\"runtime_result\":%d,\"source_eof\":%s,\"cancelled\":%s,\"retained\":%s,",
        s->source_result,s->reference_result,s->query_result,s->compute_query_result,s->allocate_queue_result,s->create_result,s->decode_result,s->flush_result,s->delete_decoder_result,s->release_queue_result,s->reference_close_result,p->source_close_result,s->close_result,s->pipeline_result,p->error,s->source_eof?"true":"false",cancelled(p)?"true":"false",p->result.retained?"true":"false");
    fprintf(f,"\"layout\":{\"verdict\":%u,\"checks\":%llu,\"matches\":%llu,\"visible_width\":%u,\"visible_height\":%u,\"native_width\":%u,\"native_height\":%u,\"pitch\":%u,\"storage_height\":%u,\"UV_offset\":%zu,\"returned_bytes\":%zu,\"owned_bytes\":%zu},",
        s->layout_verdict,(unsigned long long)s->layout_checks,(unsigned long long)s->layout_reference_matches,p->sample->visible_width,p->sample->visible_height,o->native_width,o->native_height,o->pitch,o->layout.storage_height,o->layout.uv_plane_offset,o->returned_bytes,o->owned_bytes);
    fprintf(f,"\"native_memory\":{\"query_compute\":%llu,\"query_cpu\":%llu,\"query_gpu\":%llu,\"query_shared\":%llu,\"query_output\":%llu,\"decoder_total\":%llu,\"scanout_total\":%zu,\"budget\":%u,\"decoder_policy_budget\":%u,\"scanout_policy_reservation\":%u,\"retained_decoder\":%llu,\"staging_bytes\":%llu,\"reference_scratch_only_bytes\":%llu},",
        (unsigned long long)s->queried_compute_bytes,(unsigned long long)s->queried_cpu_bytes,(unsigned long long)s->queried_gpu_bytes,(unsigned long long)s->queried_shared_bytes,(unsigned long long)s->queried_frame_bytes,(unsigned long long)s->rounded_native_bytes,p->display.bytes,PIPE_NATIVE_BUDGET,PIPE_NATIVE_BUDGET-3*SCANOUT_BYTES,3*SCANOUT_BYTES,(unsigned long long)s->allocated_native_bytes,(unsigned long long)s->staging_bytes,(unsigned long long)s->rgb_bytes);
    X4VideoPipelineDisplayOccupancy occupancy={0};
    int occupancy_rc=x4_video_pipeline_display_occupancy(&p->display.ledger,&occupancy);
    fprintf(f,"\"videoout_occupancy\":{\"scope\":\"main_loop_observations_not_time_weighted\",\"observations\":%llu,\"omitted\":0,\"peak_occupied\":%u,\"peak_in_flight\":%u,\"final_result\":%d,\"final_occupied\":%u,\"final_in_flight\":%u,\"occupied_samples\":[%llu,%llu,%llu,%llu],\"in_flight_samples\":[%llu,%llu,%llu,%llu]},",
        (unsigned long long)p->scanout_observations,p->display.ledger.peak_occupied,p->display.ledger.peak_in_flight,
        occupancy_rc,occupancy.occupied,occupancy.in_flight,
        (unsigned long long)p->scanout_occupied_samples[0],(unsigned long long)p->scanout_occupied_samples[1],
        (unsigned long long)p->scanout_occupied_samples[2],(unsigned long long)p->scanout_occupied_samples[3],
        (unsigned long long)p->scanout_inflight_samples[0],(unsigned long long)p->scanout_inflight_samples[1],
        (unsigned long long)p->scanout_inflight_samples[2],(unsigned long long)p->scanout_inflight_samples[3]);
    fprintf(f,"\"ring\":{\"peak_occupied\":%u,\"peak_ready\":%u,\"discarded_ready\":%llu,\"discarded_ring_full\":%llu,\"discarded_latency\":%llu,\"cancelled_leases\":%llu,\"waits\":%llu,\"rejected\":%llu,\"final_occupied\":%u},\"scanout\":{\"allocate\":%d,\"map\":%d,\"register\":%d,\"rate\":%d,\"close\":%d,\"unmap\":%d,\"release\":%d,\"display_error\":%s},\"reference_scores\":[",
        p->ring.peak_occupied,p->ring.peak_ready,(unsigned long long)p->ring.discarded_ready,(unsigned long long)p->ring.discarded_ready,(unsigned long long)p->ring.discarded_latency,(unsigned long long)p->ring.cancelled,(unsigned long long)p->ring.waits,(unsigned long long)p->ring.rejected,p->ring.occupied,p->display.allocate_rc,p->display.map_rc,p->display.register_rc,p->display.rate_rc,p->display.close_rc,p->display.unmap_rc,p->display.release_rc,p->display_error?"true":"false");
    for(unsigned i=0;i<3;++i) {
        if(i)fputc(',',f);
        fputc('[',f);
        for(unsigned j=0;j<p->video.probe_raw_score_counts[i];++j) {
            const X4ProbeReferenceScore *score=&p->video.probe_raw_scores[i][j];
            fprintf(f,"%s{\"MAE\":%.9f,\"max_delta\":%u,\"alpha_failures\":%llu,\"accepted_references\":%u,\"identity_ambiguous\":%s}",j?",":"",score->mean_abs,score->max_channel_delta,(unsigned long long)score->alpha_failures,score->accepted_references,score->frame_identity_ambiguous?"true":"false");
        } fputc(']',f);
    }
    fputs("],\"thread_stacks\":",f);x4_probe_metrics_stacks_json(f,p->metrics);fputs("}\n",f);
    int write_error=ferror(f)?(errno?errno:EIO):0;
    errno=0;
    int finish=x4_probe_report_finish(f), finish_errno=errno;
    if(write_error)return export_failure("pipeline_write",path,key,p->result.started_us,-write_error,write_error);
    if(finish)return export_failure("pipeline_finish",path,key,p->result.started_us,finish,finish_errno);
    printf("XCloud4 pipeline: report=%s result=0 errno=0\n",path);
    return 0;
}

X4ProbePipelineRunResult x4_probe_pipeline_run(const X4ProbeCase *sample,X4ProbeMode mode,
    unsigned converters,X4Controller *pad)
{
    X4ProbePipelineRunResult failed={.status=x4_probe_status_pending(),.stopped=true,.report_result=-1};
    if(!sample || !pad || (converters!=0 && converters!=2 && converters!=3))return failed;
    Pipeline *p=calloc(1,sizeof(*p)); if(!p)return failed;
    p->sample=sample;p->mode=mode;p->converters=converters;p->result.started_us=clock_us();
    p->video.probe_status=x4_probe_status_pending();p->source_close_result=X4_PROBE_RC_NOT_RUN;
    atomic_flag_clear(&p->gate);atomic_init(&p->cancel,false);atomic_init(&p->produced_done,false);
    atomic_init(&p->cleanup_allowed,false);atomic_init(&p->decoder_finished,false);
    atomic_init(&p->outputs,0);atomic_init(&p->conversions,0);atomic_init(&p->measurement_start,0);
    atomic_init(&p->source_ready,false);atomic_init(&p->input_eof,false);atomic_init(&p->input_error,0);
    atomic_init(&p->input_submitted,0);atomic_init(&p->input_consumed,0);
    x4_video_pipeline_ring_init(&p->ring,p->result.started_us?p->result.started_us:1);
    scanout_init(&p->display,p->ring.epoch);
    p->metrics=x4_probe_metrics_create(p->result.started_us);
    p->au[0]=malloc(X4_PROBE_AU_MAX);p->au[1]=malloc(X4_PROBE_AU_MAX);p->reference_pixels=malloc((size_t)X4_LIVE_RGB_PIXELS*4);
    int rc=(!p->metrics||!p->au[0]||!p->au[1]||!p->reference_pixels)?X4_PROBE_MEMORY:x4_probe_hash_context_init(&p->hash);
    if(!rc && converters) rc=scanout_open(&p->display);
    for(unsigned i=0;!rc && i<converters;++i) {
        Converter *w=&p->workers[i];w->owner=p;w->index=i;
        atomic_init(&w->stop,false);atomic_init(&w->finished,false);atomic_init(&w->submitted,0);atomic_init(&w->completed,0);
        static const char *const names[]={"x4-convert0","x4-convert1","x4-convert2"};
        rc=x4_probe_thread_create(&w->id,&w->thread,probe_pipeline_convert_worker,w,names[i]);
    }
    if(!rc)rc=x4_probe_thread_create(&p->decoder,&p->decoder_thread,probe_pipeline_decode_worker,p,"x4-decode");
    p->error=rc;
    bool converting=false; X4VideoPipelineLease image_lease={0},scanout_lease={0};
    X4ProbeNativeOutput image={0};uint64_t work=0,conversion_begin=0,close_deadline=0;
    for(unsigned tick=0;p->decoder_thread.created && !p->decoder_thread.report.wrapper_result;++tick) {
        control(p,pad,tick);observe_measurement(p);input_tick(p);
        if(p->error)atomic_store(&p->cancel,true);
        bool produced=atomic_load_explicit(&p->produced_done,memory_order_acquire);
        if(produced && p->video.probe_status.pipeline_result && p->video.probe_status.pipeline_result!=X4_PROBE_CANCELLED) {
            p->error=p->video.probe_status.pipeline_result;atomic_store(&p->cancel,true);
        }
        if(converting) {
            bool complete=true;
            for(unsigned i=0;i<converters;++i)if(atomic_load_explicit(&p->workers[i].completed,memory_order_acquire)!=work)complete=false;
            if(complete) {
                uint64_t end=clock_us();duration(p,X4_PROBE_STAGE_CONVERT,end,end-conversion_begin);
                for(unsigned i=0;i<converters;++i)if(p->workers[i].result)p->error=p->workers[i].result;
                if(!p->error && !cancelled(p)) {
                    p->error=x4_video_pipeline_finish_scanout(&p->display.ledger,scanout_lease,image.output_id,image.decode_end_us,end);
                    if(!p->error) {
                        ++p->convert_observed;atomic_store_explicit(&p->conversions,p->convert_observed,memory_order_release);
                        note(p,X4_PROBE_COUNT_RGB_PUBLICATION,1);note(p,X4_PROBE_COUNT_RGB_CONVERTED,1);
                        observe_measurement(p);
                    } else {
                        /* Preserve the failed transition, never publish its
                         * pixels, and retire only this unsubmitted writer. */
                        (void)x4_video_pipeline_abort_scanout(&p->display.ledger,scanout_lease);
                        atomic_store(&p->cancel,true);
                    }
                } else (void)x4_video_pipeline_abort_scanout(&p->display.ledger,scanout_lease);
                ring_lock(p);int released=x4_video_pipeline_release_conversion(&p->ring,image_lease);ring_unlock(p);
                if(released)p->error=released;
                converting=false;
            } else if(clock_us()-conversion_begin>PIPE_TIMEOUT_US) {p->error=X4_PROBE_STACK_TIMEOUT;atomic_store(&p->cancel,true);}
        }
        if(converters && !p->display_error) {
            int display_rc=display_tick(p);if(display_rc){p->error=display_rc;p->display_error=true;atomic_store(&p->cancel,true);}
        }
        if(cancelled(p)) {ring_lock(p);(void)x4_video_pipeline_cancel_ready(&p->ring);ring_unlock(p);}
        if(converters && !converting && !cancelled(p) && !p->error) {
            int free_scanout=x4_video_pipeline_acquire_scanout(&p->display.ledger,&scanout_lease);
            if(!free_scanout) {
                X4VideoPipelineLatencyDrops latency_drops={0};
                ring_lock(p);int have=x4_video_pipeline_take_ready(&p->ring,&image_lease,&latency_drops);
                if(!have)image=p->images[image_lease.slot];
                ring_unlock(p);
                /* Bounded aggregate outside the functional ring lock. The
                 * ring's owner counter survives any diagnostic try-gate loss. */
                if(latency_drops.count)note(p,X4_PROBE_COUNT_LATENCY_SUPERSEDE,latency_drops.count);
                if(!have) {
                    if(!image.layout_verified) {
                        (void)x4_video_pipeline_abort_scanout(&p->display.ledger,scanout_lease);
                        ring_lock(p);(void)x4_video_pipeline_release_conversion(&p->ring,image_lease);ring_unlock(p);
                        p->error=X4_PIPELINE_STATE;atomic_store(&p->cancel,true);
                    }
                    else {
                        conversion_begin=clock_us();++work;converting=true;
                        for(unsigned i=0;i<converters;++i) {
                            Converter *w=&p->workers[i];w->image=image;w->target=p->display.pixels[scanout_lease.slot];
                            atomic_store_explicit(&w->submitted,work,memory_order_release);
                        }
                    }
                } else (void)x4_video_pipeline_abort_scanout(&p->display.ledger,scanout_lease);
            } else if(free_scanout!=X4_PIPELINE_WAIT) {p->error=free_scanout;atomic_store(&p->cancel,true);}
        }
        ring_lock(p);unsigned ready=p->ring.ready;ring_unlock(p);
        if(produced && !converting && !ready && (!converters || p->display_error || display_drained(&p->display)))
            atomic_store_explicit(&p->cleanup_allowed,true,memory_order_release);
        if(produced || cancelled(p)) {
            if(!close_deadline)close_deadline=clock_us()+PIPE_TIMEOUT_US;
            if(clock_us()>close_deadline && !atomic_load(&p->decoder_finished)){p->result.retained=true;break;}
        }
        int occupancy_rc=observe_scanout_occupancy(p);
        if(occupancy_rc){p->error=occupancy_rc;atomic_store(&p->cancel,true);}
        if(atomic_load_explicit(&p->decoder_finished,memory_order_acquire))break;
        sceKernelUsleep(100);
    }
    atomic_store(&p->cancel,p->error?true:atomic_load(&p->cancel));
    if(p->result.retained) {p->result.stopped=true;p->result.report_result=-1;return p->result;}
    if(p->decoder_thread.created) {
        int join=scePthreadJoin(p->decoder,NULL);x4_probe_thread_joined(&p->decoder_thread,join);
        if(join){p->result.retained=true;p->result.stopped=true;return p->result;}
    }
    x4_probe_metrics_record_stack(p->metrics,0,&p->decoder_thread.report);
    for(unsigned i=0;i<converters;++i) {
        Converter *w=&p->workers[i];if(w->thread.created) {
            atomic_store_explicit(&w->stop,true,memory_order_release);
            int join=scePthreadJoin(w->id,NULL);x4_probe_thread_joined(&w->thread,join);
            if(join){p->result.retained=true;p->result.stopped=true;return p->result;}
        }
        x4_probe_metrics_record_stack(p->metrics,i+1,&w->thread.report);
    }
    rc=scanout_close(&p->display);if(rc){p->error=rc;p->result.retained=true;}
    rc=observe_scanout_occupancy(p);if(rc)p->error=rc;
    if(p->native_retained)p->result.retained=true;
    /* These fixed samples contain no B frames. Ready-output drops affect
     * presentation only; all indexed AUs must still yield native pictures. */
    if(!p->error && !cancelled(p) && p->video.probe_status.source_eof &&
       (p->video.probe_status.input_aus!=sample->au_count ||
        p->video.probe_status.native_outputs!=sample->au_count)) p->error=X4_PROBE_STATE;
    p->result.ended_us=clock_us();p->result.status=p->video.probe_status;
    p->result.status.rgb_publications=p->convert_observed;p->result.status.rgb_conversions=p->convert_observed;
    p->result.status.measured_start_us=atomic_load(&p->measurement_start);
    p->result.cancelled=cancelled(p);p->result.stopped=p->error || p->result.retained ||
        (p->result.status.pipeline_result && p->result.status.pipeline_result!=X4_PROBE_CANCELLED) || p->result.status.close_result;
    x4_probe_metrics_end(p->metrics,p->result.ended_us,p->result.stopped?X4_PROBE_END_ERROR:p->result.cancelled?X4_PROBE_END_CANCEL:X4_PROBE_END_CLEAN_EOF);
    p->result.report_result=export_pipeline(p);
    p->result.compatibility_passed=!p->result.stopped && !p->result.cancelled && !p->result.report_result &&
        p->result.status.source_eof && p->result.status.layout_verdict==X4_PROBE_LAYOUT_VERIFIED &&
        p->result.status.native_outputs>=3 && (!converters || p->presented>0);
    X4ProbePipelineRunResult result=p->result;
    if(!result.retained){free(p->reference_pixels);free(p->au[0]);free(p->au[1]);x4_probe_hash_context_close(&p->hash);x4_probe_metrics_free(p->metrics);free(p);}
    return result;
}
#endif
