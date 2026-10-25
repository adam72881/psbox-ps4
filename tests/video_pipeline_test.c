/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void invariant(const X4VideoPipelineRing *r)
{
    unsigned occupied = 0, ready = 0;
    for (unsigned i = 0; i < X4_PIPELINE_OUTPUTS; ++i) {
        assert(r->slots[i].state <= X4_PIPELINE_CONVERTING);
        occupied += r->slots[i].state != X4_PIPELINE_FREE;
        ready += r->slots[i].state == X4_PIPELINE_READY;
    }
    assert(occupied == r->occupied && ready == r->ready);
    assert(r->peak_occupied >= occupied && r->peak_occupied <= 4);
    assert(r->peak_ready >= ready && r->peak_ready <= 4);
    assert(r->acquired == r->occupied + r->converted + r->cancelled +
        r->discarded_ready + r->discarded_latency);
}
static void ring_cases(void)
{
    X4VideoPipelineRing r;
    X4VideoPipelineLease lease[4], next, held, returned;
    X4VideoPipelineLatencyDrops drops;
    uint64_t discarded = 123;
    assert(x4_video_pipeline_ring_init(&r,0) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_ring_init(&r,11) == 0);
    assert(x4_video_pipeline_ring_idle(&r));
    for (unsigned i = 0; i < 4; ++i) {
        assert(x4_video_pipeline_acquire_decode(&r,&lease[i],&discarded) == 0);
        assert(!discarded && lease[i].slot == i);
    }
    /* Four accepted native buffers with delayed output must not be reused. */
    assert(x4_video_pipeline_acquire_decode(&r,&next,&discarded) == X4_PIPELINE_WAIT);
    assert(r.occupied == 4 && !next.generation && !discarded);
    assert(x4_video_pipeline_decoding_lease(&r,lease[2].slot,&returned) == 0);
    assert(returned.generation == lease[2].generation);
    assert(x4_video_pipeline_publish_ready(&r,returned,101,1000) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[0],102,1001) == 0);
    assert(x4_video_pipeline_take_ready(&r,&held,&drops) == 0);
    assert(held.slot == 0); /* Latest publication, not the first returned slot. */
    assert(drops.count == 1 && drops.output_ids[0] == 101);
    assert(x4_video_pipeline_acquire_decode(&r,&next,&discarded) == 0);
    assert(discarded == 0 && next.slot == 2);
    assert(r.slots[0].state == X4_PIPELINE_CONVERTING);
    assert(r.slots[1].state == X4_PIPELINE_DECODING);
    assert(r.discarded_ready == 0 && r.discarded_latency == 1 && r.occupied == 4);
    assert(x4_video_pipeline_publish_ready(&r,lease[2],900,1002) == X4_PIPELINE_STALE);
    assert(x4_video_pipeline_release_conversion(&r,lease[1]) == X4_PIPELINE_STATE);
    assert(x4_video_pipeline_publish_ready(&r,held,900,1002) == X4_PIPELINE_STATE);
    assert(x4_video_pipeline_cancel_ready(&r) == 0);
    assert(x4_video_pipeline_acquire_decode(&r,&returned,NULL) == X4_PIPELINE_WAIT);
    assert(x4_video_pipeline_release_conversion(&r,held) == 0);
    assert(x4_video_pipeline_release_conversion(&r,held) == X4_PIPELINE_STATE);
    assert(x4_video_pipeline_abort_decode(&r,next) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[1],103,1003) == 0);
    assert(x4_video_pipeline_cancel_ready(&r) == 1);
    assert(x4_video_pipeline_abort_decode(&r,lease[3]) == 0);
    assert(x4_video_pipeline_ring_idle(&r)); invariant(&r);
    assert(x4_video_pipeline_ring_init(&r,12) == 0);
    assert(x4_video_pipeline_acquire_decode(&r,&next,NULL) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[0],1,0) == X4_PIPELINE_STALE);
    assert(x4_video_pipeline_abort_decode(&r,next) == 0);
    r.next_generation = UINT64_MAX;
    assert(x4_video_pipeline_acquire_decode(&r,&next,NULL) == X4_PIPELINE_EXHAUSTED);
    assert(x4_video_pipeline_ring_idle(&r));
    puts("PASS output ring: delayed native output, newest READY, ownership, stale session and rollover");
}
static void latest_ready_cases(void)
{
    X4VideoPipelineRing r;
    X4VideoPipelineLease lease[4], picked, next;
    X4VideoPipelineLatencyDrops drops;
    uint64_t discard = 0;
    assert(x4_video_pipeline_ring_init(&r,44) == 0);
    memset(&drops,0xff,sizeof(drops));
    assert(x4_video_pipeline_take_ready(&r,&picked,&drops) == X4_PIPELINE_WAIT);
    assert(!picked.generation && drops.count == 0);
    for (unsigned i = 0; i < 3; ++i) assert(drops.output_ids[i] == 0);
    memset(&drops,0xff,sizeof(drops));
    assert(x4_video_pipeline_take_ready(NULL,&picked,&drops) == X4_PIPELINE_INVALID && drops.count == 0);
    for (unsigned i = 0; i < 4; ++i) assert(x4_video_pipeline_acquire_decode(&r,&lease[i],NULL) == 0);
    /* IDs, timestamps and physical slots deliberately disagree with READY
     * order. A delayed native return is eligible only when actually published. */
    assert(x4_video_pipeline_publish_ready(&r,lease[2],900,9000) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[0],700,7000) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[3],500,5000) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[1],100,1000) == 0);
    assert(x4_video_pipeline_take_ready(&r,&picked,&drops) == 0);
    assert(picked.slot == 1 && r.slots[picked.slot].output_id == 100);
    assert(drops.count == 3 && drops.output_ids[0] == 900 &&
        drops.output_ids[1] == 700 && drops.output_ids[2] == 500);
    assert(r.discarded_latency == 3 && r.discarded_ready == 0 &&
        r.cancelled == 0 && r.converted == 0 && r.occupied == 1 && r.ready == 0);
    invariant(&r);
    assert(x4_video_pipeline_release_conversion(&r,lease[2]) == X4_PIPELINE_STATE);
    assert(x4_video_pipeline_acquire_decode(&r,&next,&discard) == 0 && discard == 0 && next.slot == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[0],1,1) == X4_PIPELINE_STALE);
    assert(x4_video_pipeline_publish_ready(&r,next,77,1200) == 0);
    /* Cancelling READY must leave an in-progress conversion untouched. */
    assert(x4_video_pipeline_cancel_ready(&r) == 1);
    assert(r.slots[picked.slot].state == X4_PIPELINE_CONVERTING);
    assert(r.discarded_latency == 3 && r.cancelled == 1 && r.occupied == 1);
    assert(x4_video_pipeline_release_conversion(&r,picked) == 0);
    assert(x4_video_pipeline_ring_idle(&r)); invariant(&r);
    assert(x4_video_pipeline_ring_init(&r,45) == 0);
    for (unsigned i = 0; i < 4; ++i) assert(x4_video_pipeline_acquire_decode(&r,&lease[i],NULL) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[0],201,20) == 0);
    assert(x4_video_pipeline_take_ready(&r,&picked,NULL) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[1],202,21) == 0);
    assert(x4_video_pipeline_publish_ready(&r,lease[2],203,22) == 0);
    /* Full-ring admission still drops ONLY the oldest READY. */
    X4VideoPipelineOutput held = r.slots[picked.slot], native = r.slots[3];
    assert(x4_video_pipeline_acquire_decode(&r,&next,&discard) == 0);
    assert(discard == 202 && next.slot == 1 && r.discarded_ready == 1 && r.discarded_latency == 0);
    assert(memcmp(&held,&r.slots[picked.slot],sizeof(held)) == 0);
    assert(memcmp(&native,&r.slots[3],sizeof(native)) == 0);
    assert(x4_video_pipeline_publish_ready(&r,next,204,23) == 0);
    X4VideoPipelineLease second;
    assert(x4_video_pipeline_take_ready(&r,&second,NULL) == 0);
    assert(second.slot == 1 && r.discarded_latency == 1 && r.discarded_ready == 1);
    assert(memcmp(&held,&r.slots[picked.slot],sizeof(held)) == 0);
    assert(memcmp(&native,&r.slots[3],sizeof(native)) == 0);
    assert(x4_video_pipeline_release_conversion(&r,second) == 0);
    assert(x4_video_pipeline_release_conversion(&r,picked) == 0);
    assert(x4_video_pipeline_abort_decode(&r,lease[3]) == 0);
    assert(x4_video_pipeline_ring_idle(&r)); invariant(&r);
    puts("PASS latest READY: publication order, three bounded IDs, NULL diagnostics, separate reasons and held ownership");
}

/* Test-only exact FIFO selection from frozen 930f. Never linked into native
 * candidate code. Both simulations below receive the identical clock/events. */
static int retained_fifo_take(X4VideoPipelineRing *r, X4VideoPipelineLease *out)
{
    int selected = -1;
    for (unsigned i = 0; i < 4; ++i)
        if (r->slots[i].state == X4_PIPELINE_READY &&
            (selected < 0 || r->slots[i].ready_order < r->slots[selected].ready_order)) selected = (int)i;
    if (selected < 0) return X4_PIPELINE_WAIT;
    r->slots[selected].state = X4_PIPELINE_CONVERTING; --r->ready;
    *out = (X4VideoPipelineLease){(unsigned)selected,r->epoch,r->slots[selected].generation};
    return 0;
}
typedef struct { uint64_t age[11], output_id[11], latency_drops, capacity_drops; } FreshnessRun;
static FreshnessRun freshness_run(bool newest)
{
    X4VideoPipelineRing r; X4VideoPipelineLease selected = {0};
    FreshnessRun result = {0}; unsigned count = 0;
    assert(x4_video_pipeline_ring_init(&r,51) == 0);
    for (uint64_t time = 0; time <= 300000; time += 10000) {
        X4VideoPipelineLease native;
        assert(x4_video_pipeline_acquire_decode(&r,&native,NULL) == 0);
        assert(x4_video_pipeline_publish_ready(&r,native,time/10000+1,time) == 0);
        if (time % 30000 == 0) {
            if (selected.generation) assert(x4_video_pipeline_release_conversion(&r,selected) == 0);
            if (newest) {
                X4VideoPipelineLatencyDrops drops;
                assert(x4_video_pipeline_take_ready(&r,&selected,&drops) == 0 && drops.count <= 3);
            } else assert(retained_fifo_take(&r,&selected) == 0);
            result.age[count] = time - r.slots[selected.slot].decode_end_us;
            result.output_id[count++] = r.slots[selected.slot].output_id;
        }
        invariant(&r);
    }
    assert(count == 11);
    result.latency_drops = r.discarded_latency; result.capacity_drops = r.discarded_ready;
    assert(x4_video_pipeline_release_conversion(&r,selected) == 0);
    x4_video_pipeline_cancel_ready(&r);
    assert(x4_video_pipeline_ring_idle(&r)); invariant(&r);
    return result;
}
static void freshness_cases(void)
{
    FreshnessRun fifo = freshness_run(false), newest = freshness_run(true);
    FreshnessRun replay = freshness_run(true);
    assert(memcmp(&newest,&replay,sizeof(newest)) == 0);
    assert(fifo.age[0] == 0 && newest.age[0] == 0);
    for (unsigned i = 1; i < 11; ++i) {
        assert(fifo.age[i] == 20000 && newest.age[i] == 0);
        assert(newest.output_id[i] == i*3+1 && fifo.output_id[i] == i*3-1);
    }
    assert(newest.latency_drops == 20 && newest.capacity_drops == 0);
    assert(fifo.latency_drops == 0 && fifo.capacity_drops > 0);
    puts("PASS fixed-clock comparison: FIFO holds 20ms native-output age; newest READY selects current output; no timing claim for PS4");
}
static uint32_t random_state = 0x9ac434;
static unsigned random_next(void)
{ random_state = random_state * 1664525u + 1013904223u; return random_state; }
static void ring_replay(void)
{
    X4VideoPipelineRing r;
    uint64_t output_id = 0;
    assert(x4_video_pipeline_ring_init(&r,23) == 0);
    for (unsigned tick = 0; tick < 100000; ++tick) {
        unsigned slot = (random_next() >> 8) % 4;
        X4VideoPipelineLease lease = {slot,r.epoch,r.slots[slot].generation};
        X4VideoPipelineOutput before[4]; memcpy(before,r.slots,sizeof(before));
        unsigned op = (random_next() >> 12) % 6;
        if (op == 0) {
            X4VideoPipelineLease taken; uint64_t discarded;
            int rc = x4_video_pipeline_acquire_decode(&r,&taken,&discarded);
            assert(rc == 0 || rc == X4_PIPELINE_WAIT);
            for (unsigned i = 0; i < 4; ++i)
                if (before[i].state == X4_PIPELINE_DECODING || before[i].state == X4_PIPELINE_CONVERTING)
                    assert(memcmp(&before[i],&r.slots[i],sizeof(before[i])) == 0);
        } else if (op == 1 && r.slots[slot].state == X4_PIPELINE_DECODING) {
            assert(x4_video_pipeline_publish_ready(&r,lease,++output_id,tick) == 0);
        } else if (op == 2) {
            X4VideoPipelineLease taken;
            X4VideoPipelineLatencyDrops drops;
            int rc = x4_video_pipeline_take_ready(&r,&taken,&drops);
            assert(rc == 0 || rc == X4_PIPELINE_WAIT);
            assert(drops.count <= 3);
            for (unsigned i = 0; i < 4; ++i)
                if (before[i].state == X4_PIPELINE_DECODING || before[i].state == X4_PIPELINE_CONVERTING)
                    assert(memcmp(&before[i],&r.slots[i],sizeof(before[i])) == 0);
            if (!rc) {
                assert(r.ready == 0 && r.slots[taken.slot].state == X4_PIPELINE_CONVERTING);
                for (unsigned i = 0; i < 4; ++i)
                    if (before[i].state == X4_PIPELINE_READY)
                        assert(before[i].ready_order <= r.slots[taken.slot].ready_order);
            }
        } else if (op == 3 && r.slots[slot].state == X4_PIPELINE_CONVERTING) {
            assert(x4_video_pipeline_release_conversion(&r,lease) == 0);
        } else if (op == 4 && r.slots[slot].state == X4_PIPELINE_DECODING) {
            assert(x4_video_pipeline_abort_decode(&r,lease) == 0);
        } else if (op == 5) x4_video_pipeline_cancel_ready(&r);
        invariant(&r);
    }
    assert(r.discarded_ready > 0 && r.waits > 0 && r.converted > 0);
    puts("PASS deterministic replay: 100000 bounded ring transitions; held buffers never overwritten");
}
static void display_cases(void)
{
    X4VideoPipelineDisplay d;
    X4VideoPipelineLease a,b,ready,presented,other;
    X4VideoPipelineDisplayOccupancy occupancy;
    assert(x4_video_pipeline_display_init(&d,31) == 0);
    assert(x4_video_pipeline_acquire_scanout(&d,&a) == 0);
    assert(x4_video_pipeline_acquire_scanout(&d,&other) == X4_PIPELINE_WAIT);
    assert(x4_video_pipeline_display_closed(&d) == X4_PIPELINE_STATE);
    assert(x4_video_pipeline_finish_scanout(&d,a,101,10,20) == 0);
    assert(x4_video_pipeline_acquire_scanout(&d,&other) == X4_PIPELINE_WAIT);
    assert(x4_video_pipeline_ready_scanout(&d,&ready) == 0 && ready.slot == a.slot);
    assert(x4_video_pipeline_mark_submitted(&d,ready,1,21) == 0);
    assert(x4_video_pipeline_acquire_scanout(&d,&b) == X4_PIPELINE_WAIT);
    assert(x4_video_pipeline_ready_scanout(&d,&ready) == X4_PIPELINE_WAIT);
    assert(x4_video_pipeline_cancel_ready_scanout(&d)==0);
    assert(x4_video_pipeline_observe_flip(&d,0,24,&presented) == X4_PIPELINE_WAIT);
    assert(d.slots[a.slot].state == X4_SCANOUT_PENDING);
    /* A delayed or stale flip observation does not unlock conversion. */
    for(unsigned i=0;i<100;++i) {
        assert(x4_video_pipeline_observe_flip(&d,999,100+i,&presented)==X4_PIPELINE_WAIT);
        assert(x4_video_pipeline_acquire_scanout(&d,&b)==X4_PIPELINE_WAIT);
    }
    assert(x4_video_pipeline_observe_flip(&d,1,24,&presented) == 0 && presented.slot == a.slot);
    assert(x4_video_pipeline_acquire_scanout(&d,&b) == 0 && b.slot != a.slot);
    assert(x4_video_pipeline_finish_scanout(&d,b,102,24,25) == 0);
    assert(x4_video_pipeline_ready_scanout(&d,&ready) == 0 && ready.slot == b.slot);
    assert(x4_video_pipeline_mark_submitted(&d,ready,1,25) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_mark_submitted(&d,ready,2,25) == 0);
    assert(x4_video_pipeline_acquire_scanout(&d,&other) == X4_PIPELINE_WAIT);
    assert(x4_video_pipeline_abort_scanout(&d,a) == X4_PIPELINE_STATE);
    assert(x4_video_pipeline_abort_scanout(&d,b) == X4_PIPELINE_STATE);
    assert(x4_video_pipeline_observe_flip(&d,1,26,&presented) == X4_PIPELINE_WAIT);
    assert(d.slots[a.slot].state == X4_SCANOUT_SCREEN);
    assert(x4_video_pipeline_observe_flip(&d,2,24,&presented) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_observe_flip(&d,2,26,&presented) == 0);
    assert(d.slots[a.slot].state == X4_SCANOUT_FREE && d.slots[b.slot].state == X4_SCANOUT_SCREEN);
    assert(x4_video_pipeline_acquire_scanout(&d,&other) == 0 && other.slot == a.slot);
    assert(x4_video_pipeline_abort_scanout(&d,a) == X4_PIPELINE_STALE);
    assert(x4_video_pipeline_abort_scanout(&d,other) == 0);
    assert(x4_video_pipeline_acquire_scanout(&d,&other)==0);
    assert(x4_video_pipeline_finish_scanout(&d,other,103,27,30)==0);
    assert(x4_video_pipeline_cancel_ready_scanout(&d)==1);
    assert(d.slots[b.slot].state==X4_SCANOUT_SCREEN);
    assert(x4_video_pipeline_display_occupancy(&d,&occupancy)==0);
    assert(occupancy.in_flight==0 && occupancy.screen==1 && occupancy.occupied==1);
    assert(d.peak_occupied==2 && d.peak_in_flight==1);
    assert(x4_video_pipeline_display_closed(&d)==0);
    assert(x4_video_pipeline_display_occupancy(&d,&occupancy)==0 && occupancy.free==3);
    a.epoch = 30;
    assert(x4_video_pipeline_finish_scanout(&d,a,1,0,0) == X4_PIPELINE_STALE);
    puts("PASS one frame in flight: SCREEN/PENDING protected, READY blocks conversion, delayed flips, cancellation and close");
}
static unsigned clamp_ref(int x) { return x < 0 ? 0 : x > 255 ? 255 : (unsigned)x; }
static uint32_t pixel_ref(const uint8_t *src, const X4VideoNv12Layout *l, unsigned x, unsigned y)
{
    int c = src[y*l->pitch+x] - 16;
    size_t uv = l->uv_plane_offset + (y/2)*l->pitch + (x&~1u);
    int d = src[uv] - 128, e = src[uv+1] - 128;
    if (c < 0) c = 0;
    unsigned r = clamp_ref((298*c+409*e+128)>>8), g = clamp_ref((298*c-100*d-208*e+128)>>8);
    unsigned b = clamp_ref((298*c+516*d+128)>>8);
    return 0xff000000u | r | g<<8 | b<<16;
}
static X4VideoNv12Layout layout_for(unsigned width, unsigned pitch)
{
    unsigned visible = width == 1280 ? 720 : 1080, storage = width == 1280 ? 720 : 1088;
    size_t y = (size_t)pitch * storage, pixels = (size_t)width * visible;
    return (X4VideoNv12Layout){.visible_width=width,.visible_height=visible,
        .coded_width=width,.coded_height=storage,.storage_height=storage,.pitch=pitch,
        .y_bytes=y,.uv_bytes=y/2,.uv_plane_offset=y,.total_nv12_bytes=y+y/2,
        .visible_y_offset=0,.visible_uv_offset=y,.rgb_pixels=pixels,.rgb_bytes=pixels*4};
}
typedef struct {
    const X4VideoNv12Layout *layout;
    const uint8_t *source;
    uint32_t *destination;
    size_t bytes;
    unsigned pitch, workers, index;
    int result;
} StripeJob;
static void *stripe_thread(void *p)
{
    StripeJob *j = p;
    j->result = x4_video_pipeline_convert_stripe(j->layout,j->source,j->layout->total_nv12_bytes,
        j->destination,j->bytes,j->pitch,j->workers,j->index);
    return NULL;
}
static void conversion_case(unsigned width, unsigned pitch, unsigned workers)
{
    X4VideoNv12Layout l = layout_for(width,pitch);
    size_t out_pitch = 1936, out_pixels = out_pitch*1080, guard = 16;
    size_t out_bytes = out_pixels*4;
    uint8_t *src = malloc(l.total_nv12_bytes);
    uint32_t *whole = malloc(out_bytes+guard*8), *reference = malloc(l.rgb_bytes);
    assert(src && whole && reference);
    uint32_t *dst = whole+guard;
    for (size_t i = 0; i < l.total_nv12_bytes; ++i) src[i] = (uint8_t)(i*73u + (i>>7)*29u);
    for (size_t i = 0; i < out_pixels+guard*2; ++i) whole[i] = 0x5aa53cc3;
    assert(x4_video_pipeline_convert_reference(&l,src,l.total_nv12_bytes,reference,l.rgb_bytes) == 0);
    for (unsigned y = 0; y < l.visible_height; ++y)
        for (unsigned x = 0; x < width; ++x)
            assert(reference[(size_t)y*width+x] == pixel_ref(src,&l,x,y));
    pthread_t threads[3]; StripeJob jobs[3]; pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr,256*1024) == 0);
    for (unsigned i = 0; i < workers; ++i) {
        jobs[i] = (StripeJob){&l,src,dst,out_bytes,(unsigned)out_pitch,workers,i,-999};
        assert(pthread_create(&threads[i],&attr,stripe_thread,&jobs[i]) == 0);
    }
    for (unsigned i = 0; i < workers; ++i) {
        assert(pthread_join(threads[i],NULL) == 0 && jobs[i].result == 0);
    }
    assert(pthread_attr_destroy(&attr) == 0);
    /* Independent retained pipeline model: first 1:1 RGB, then the exact old
     * live_media_draw floor-coordinate scaling, including every border pixel. */
    for (unsigned y = 0; y < 1080; ++y) {
        for (unsigned x = 0; x < 1920; ++x) {
            unsigned sx = x*width/1920, sy = y*l.visible_height/1080;
            assert(dst[y*out_pitch+x] == reference[(size_t)sy*width+sx]);
        }
        for (size_t x = 1920; x < out_pitch; ++x) assert(dst[y*out_pitch+x] == 0x5aa53cc3);
    }
    for (size_t i = 0; i < guard; ++i) {
        assert(whole[i] == 0x5aa53cc3 && dst[out_pixels+i] == 0x5aa53cc3);
    }
    if (width == 1920) {
        memset(src+1080*pitch,0xfe,(1088-1080)*pitch);
        memset(src+l.uv_plane_offset+540*pitch,0x01,(544-540)*pitch);
        for (unsigned i = 0; i < workers; ++i) {
            assert(x4_video_pipeline_convert_stripe(&l,src,l.total_nv12_bytes,dst,out_bytes,
                (unsigned)out_pitch,workers,i) == 0);
        }
        for (unsigned y = 0; y < 1080; ++y)
            for (unsigned x = 0; x < 1920; ++x) assert(dst[y*out_pitch+x] == reference[y*1920+x]);
    }
    X4VideoNv12Layout bad = l;
    assert(x4_video_pipeline_convert_stripe(&l,src,l.total_nv12_bytes-1,dst,out_bytes,
        (unsigned)out_pitch,workers,0) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_convert_stripe(&l,src,l.total_nv12_bytes,dst,out_bytes-1,
        (unsigned)out_pitch,workers,0) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_convert_stripe(&l,src,l.total_nv12_bytes,(uint32_t *)src,l.total_nv12_bytes,
        1920,workers,0) == X4_PIPELINE_INVALID);
    bad.visible_uv_offset -= 1;
    assert(x4_video_pipeline_convert_reference(&bad,src,l.total_nv12_bytes,reference,l.rgb_bytes) == X4_PIPELINE_INVALID);
    bad = l; bad.pitch |= 1;
    assert(x4_video_pipeline_convert_reference(&bad,src,l.total_nv12_bytes,reference,l.rgb_bytes) == X4_PIPELINE_INVALID);
    bad = l; bad.storage_height -= 8;
    assert(x4_video_pipeline_convert_reference(&bad,src,l.total_nv12_bytes,reference,l.rgb_bytes) == X4_PIPELINE_INVALID);
    bad = l; bad.visible_y_offset = pitch*2;
    assert(x4_video_pipeline_convert_reference(&bad,src,l.total_nv12_bytes,reference,l.rgb_bytes) == X4_PIPELINE_INVALID);
    bad = l; bad.total_nv12_bytes = SIZE_MAX;
    assert(x4_video_pipeline_convert_reference(&bad,src,l.total_nv12_bytes,reference,l.rgb_bytes) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_convert_reference(NULL,src,l.total_nv12_bytes,reference,l.rgb_bytes) == X4_PIPELINE_INVALID);
    free(reference); free(whole); free(src);
    printf("PASS direct conversion %ux%u pitch=%u workers=%u: every pixel equals legacy, padding/guards intact\n",
        width,l.visible_height,pitch,workers);
}
static void stripe_cases(void)
{
    X4VideoPipelineStripe stripe;
    for (unsigned workers = 2; workers <= 3; ++workers) {
        unsigned end = 0;
        for (unsigned i = 0; i < workers; ++i) {
            assert(x4_video_pipeline_stripe(1080,workers,i,&stripe) == 0);
            assert(stripe.first_row == end && stripe.end_row > stripe.first_row);
            end = stripe.end_row;
        }
        assert(end == 1080);
        conversion_case(1280,1280,workers);
        conversion_case(1920,1920,workers);
        conversion_case(1920,2048,workers);
    }
    assert(x4_video_pipeline_stripe(1080,1,0,&stripe) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_stripe(1080,4,0,&stripe) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_stripe(1080,2,2,&stripe) == X4_PIPELINE_INVALID);
    assert(x4_video_pipeline_stripe(UINT32_MAX,3,0,&stripe) == X4_PIPELINE_INVALID);
    puts("PASS stripe partition: two/three disjoint contiguous ranges and fail-closed invalid configuration");
}
int main(void)
{
    ring_cases(); latest_ready_cases(); freshness_cases(); ring_replay(); display_cases(); stripe_cases();
    puts("PASS portable pipeline tests; no PS4 timing/coherence claim");
    return 0;
}
