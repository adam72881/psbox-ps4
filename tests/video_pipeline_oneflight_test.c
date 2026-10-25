/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <stdio.h>

/* Rational clock: 42000 ticks/s gives exact 60-Hz vsync (700 ticks),
 * 70-fps native output (600 ticks), and 5-ms conversion (210 ticks).
 * The old scheduling model allows one converted READY behind PENDING, as the
 * old triple-buffer steady state did. Its cold-start extra capacity is omitted
 * from both rates by a 0.5-s warmup; this is not a PS4 timing simulation. */
enum { SECOND=42000, VSYNC=700, DECODE=600, CONVERT=210,
       WARMUP=SECOND/2, END=SECOND*5 };
typedef struct {
    X4VideoPipelineRing ring;
    X4VideoPipelineDisplay display;
    X4VideoPipelineLease native, target;
    bool writing, ready, pending;
    uint64_t conversion_end, converted_at, pending_decode, ready_decode;
    uint64_t submitted_at, argument, output_id;
    uint64_t presents, ready_wait_sum, ready_wait_max, decode_age_sum;
    unsigned peak_old_inflight;
} Model;

static void model_native(Model *m,uint64_t now)
{
    X4VideoPipelineLease lease;uint64_t discard;
    assert(x4_video_pipeline_acquire_decode(&m->ring,&lease,&discard)==0);
    assert(x4_video_pipeline_publish_ready(&m->ring,lease,++m->output_id,now)==0);
}
static void model(bool oneflight,Model *m)
{
    assert(x4_video_pipeline_ring_init(&m->ring,1)==0);
    assert(x4_video_pipeline_display_init(&m->display,1)==0);
    for(uint64_t now=0;now<=END;++now) {
        if(now%DECODE==0)model_native(m,now);
        if(m->writing && now==m->conversion_end) {
            m->writing=false;m->ready=true;m->converted_at=now;
            if(oneflight)assert(x4_video_pipeline_finish_scanout(&m->display,m->target,
                m->ring.slots[m->native.slot].output_id,m->ready_decode,now)==0);
            assert(x4_video_pipeline_release_conversion(&m->ring,m->native)==0);
        }
        if(m->pending && now%VSYNC==0 && now>m->submitted_at) {
            if(oneflight) {
                X4VideoPipelineLease shown;
                assert(x4_video_pipeline_observe_flip(&m->display,m->argument,now,&shown)==0);
            }
            if(now>WARMUP) {++m->presents;m->decode_age_sum+=now-m->pending_decode;}
            m->pending=false;
        }
        if(m->ready && !m->pending) {
            uint64_t wait=now-m->converted_at;
            if(now>WARMUP) {
                m->ready_wait_sum+=wait;
                if(wait>m->ready_wait_max)m->ready_wait_max=wait;
            }
            m->pending_decode=m->ready_decode;m->submitted_at=now;++m->argument;
            if(oneflight) {
                X4VideoPipelineLease ready;
                assert(x4_video_pipeline_ready_scanout(&m->display,&ready)==0);
                assert(x4_video_pipeline_mark_submitted(&m->display,ready,m->argument,now)==0);
            }
            m->ready=false;m->pending=true;
        }
        if(!m->writing && !m->ready && (!oneflight || !m->pending) && m->ring.ready) {
            if(oneflight)assert(x4_video_pipeline_acquire_scanout(&m->display,&m->target)==0);
            X4VideoPipelineLatencyDrops drops;
            assert(x4_video_pipeline_take_ready(&m->ring,&m->native,&drops)==0);
            m->ready_decode=m->ring.slots[m->native.slot].decode_end_us;
            m->writing=true;m->conversion_end=now+CONVERT;
        }
        unsigned active=(unsigned)m->writing+(unsigned)m->ready+(unsigned)m->pending;
        if(active>m->peak_old_inflight)m->peak_old_inflight=active;
        if(oneflight) {
            X4VideoPipelineDisplayOccupancy o;
            assert(x4_video_pipeline_display_occupancy(&m->display,&o)==0);
            assert(o.in_flight<=1 && o.occupied<=2 && !(o.ready && o.pending));
            assert(o.in_flight==active);
        }
    }
    /* Cancel finishes an owned conversion before releasing its native slot;
     * a real caller must join converters and close VideoOut before closed(). */
    if(m->writing) {
        if(oneflight)assert(x4_video_pipeline_abort_scanout(&m->display,m->target)==0);
        assert(x4_video_pipeline_release_conversion(&m->ring,m->native)==0);
    }
    x4_video_pipeline_cancel_ready(&m->ring);
    assert(x4_video_pipeline_ring_idle(&m->ring));
    if(oneflight)assert(x4_video_pipeline_display_closed(&m->display)==0);
}

int main(void)
{
    Model old={0},fresh={0};model(false,&old);model(true,&fresh);
    assert(old.presents==270 && fresh.presents==old.presents);
    assert(old.ready_wait_max==VSYNC-CONVERT && old.ready_wait_sum>old.presents*400);
    assert(fresh.ready_wait_sum==0 && fresh.ready_wait_max==0);
    assert(old.peak_old_inflight==2 && fresh.display.peak_in_flight==1);
    assert(fresh.decode_age_sum<old.decode_age_sum);
    printf("PASS rational model: 60Hz/70fps/5ms; old/new measured presents=%llu/%llu over 4.5s (60/60 fps); old READY wait max=%.6f ms, new=0 ms; mean native-end age %.6f -> %.6f ms; synthetic only\n",
        (unsigned long long)old.presents,(unsigned long long)fresh.presents,
        (double)old.ready_wait_max/42.0,
        (double)old.decode_age_sum/(double)old.presents/42.0,
        (double)fresh.decode_age_sum/(double)fresh.presents/42.0);
    return 0;
}
