/* SPDX-License-Identifier: GPL-3.0-only */
#include "video_pipeline.h"
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__SSE2__) && !defined(X4_PIPELINE_SCALAR)
#define X4_PIPELINE_SSE2 1
#else
#define X4_PIPELINE_SSE2 0
#endif

#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
#define OUTPUT_LIMIT(r) ((r)->output_count)
#else
#define OUTPUT_LIMIT(r) X4_PIPELINE_OUTPUTS
#endif

static void clear_lease(X4VideoPipelineLease *lease)
{ if (lease) memset(lease, 0, sizeof(*lease)); }
int x4_video_pipeline_ring_init(X4VideoPipelineRing *r, uint64_t epoch)
{
    if (!r || !epoch) return X4_PIPELINE_INVALID;
    memset(r, 0, sizeof(*r)); r->epoch = epoch;
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
    r->output_count = X4_PIPELINE_OUTPUTS;
#endif
    return 0;
}
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
int x4_video_pipeline_ring_init_outputs(X4VideoPipelineRing *r, uint64_t epoch,
    unsigned count)
{
    if (count != 4 && count != 5) return X4_PIPELINE_INVALID;
    int rc = x4_video_pipeline_ring_init(r, epoch);
    if (!rc) r->output_count = count;
    return rc;
}
#endif
static X4VideoPipelineLease output_lease(const X4VideoPipelineRing *r, unsigned i)
{ return (X4VideoPipelineLease){i, r->epoch, r->slots[i].generation}; }
static int output_check(X4VideoPipelineRing *r, X4VideoPipelineLease lease,
    X4VideoPipelineOutputState state)
{
    if (!r || !r->epoch || lease.slot >= OUTPUT_LIMIT(r)) return X4_PIPELINE_INVALID;
    if (lease.epoch != r->epoch || !lease.generation ||
        lease.generation != r->slots[lease.slot].generation) {
        ++r->rejected; return X4_PIPELINE_STALE;
    }
    if (r->slots[lease.slot].state != state) { ++r->rejected; return X4_PIPELINE_STATE; }
    return 0;
}
static int oldest_ready(const X4VideoPipelineRing *r)
{
    int best = -1;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_READY &&
            (best < 0 || r->slots[i].ready_order < r->slots[best].ready_order)) best = (int)i;
    return best;
}
int x4_video_pipeline_acquire_decode(X4VideoPipelineRing *r,
    X4VideoPipelineLease *out, uint64_t *discarded_output_id)
{
    clear_lease(out);
    if (discarded_output_id) *discarded_output_id = 0;
    if (!r || !r->epoch || !out) return X4_PIPELINE_INVALID;
    if (r->next_generation == UINT64_MAX) return X4_PIPELINE_EXHAUSTED;
    int slot = -1;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_FREE) { slot = (int)i; break; }
    if (slot < 0) {
        slot = oldest_ready(r);
        if (slot < 0) { ++r->waits; return X4_PIPELINE_WAIT; }
        if (discarded_output_id) *discarded_output_id = r->slots[slot].output_id;
        ++r->discarded_ready; --r->ready;
    } else {
        if (++r->occupied > r->peak_occupied) r->peak_occupied = r->occupied;
    }
    r->slots[slot] = (X4VideoPipelineOutput){
        .state = X4_PIPELINE_DECODING, .generation = ++r->next_generation};
    ++r->acquired; *out = output_lease(r, (unsigned)slot);
    return 0;
}
int x4_video_pipeline_decoding_lease(X4VideoPipelineRing *r, unsigned slot,
    X4VideoPipelineLease *out)
{
    clear_lease(out);
    if (!r || !r->epoch || !out || slot >= OUTPUT_LIMIT(r)) return X4_PIPELINE_INVALID;
    if (r->slots[slot].state != X4_PIPELINE_DECODING) { ++r->rejected; return X4_PIPELINE_STATE; }
    *out = output_lease(r, slot); return 0;
}
int x4_video_pipeline_publish_ready(X4VideoPipelineRing *r, X4VideoPipelineLease lease,
    uint64_t output_id, uint64_t decode_end_us)
{
    int rc = output_check(r, lease, X4_PIPELINE_DECODING);
    if (rc) return rc;
    if (!output_id) return X4_PIPELINE_INVALID;
    if (r->next_ready_order == UINT64_MAX) return X4_PIPELINE_EXHAUSTED;
    X4VideoPipelineOutput *s = &r->slots[lease.slot];
    s->state = X4_PIPELINE_READY; s->output_id = output_id;
    s->decode_end_us = decode_end_us; s->ready_order = ++r->next_ready_order;
    ++r->published;
    if (++r->ready > r->peak_ready) r->peak_ready = r->ready;
    return 0;
}
int x4_video_pipeline_take_ready(X4VideoPipelineRing *r, X4VideoPipelineLease *out,
    X4VideoPipelineLatencyDrops *drops)
{
    clear_lease(out);
    if (drops) memset(drops, 0, sizeof(*drops));
    if (!r || !r->epoch || !out) return X4_PIPELINE_INVALID;
    int slot = -1;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_READY &&
            (slot < 0 || r->slots[i].ready_order > r->slots[slot].ready_order)) slot = (int)i;
    if (slot < 0) return X4_PIPELINE_WAIT;
    r->slots[slot].state = X4_PIPELINE_CONVERTING; --r->ready;
    /* The selected slot is no longer READY; only unclaimed older publications
     * can be released. The caller's ring lock covers selection and every drop. */
    int old;
    while ((old = oldest_ready(r)) >= 0) {
        if (drops) drops->output_ids[drops->count++] = r->slots[old].output_id;
        r->slots[old].state = X4_PIPELINE_FREE;
        --r->ready; --r->occupied; ++r->discarded_latency;
    }
    *out = output_lease(r, (unsigned)slot); return 0;
}
int x4_video_pipeline_release_conversion(X4VideoPipelineRing *r, X4VideoPipelineLease lease)
{
    int rc = output_check(r, lease, X4_PIPELINE_CONVERTING);
    if (rc) return rc;
    r->slots[lease.slot].state = X4_PIPELINE_FREE;
    --r->occupied; ++r->converted; return 0;
}
int x4_video_pipeline_abort_decode(X4VideoPipelineRing *r, X4VideoPipelineLease lease)
{
    int rc = output_check(r, lease, X4_PIPELINE_DECODING);
    if (rc) return rc;
    r->slots[lease.slot].state = X4_PIPELINE_FREE;
    --r->occupied; ++r->cancelled; return 0;
}
unsigned x4_video_pipeline_cancel_ready(X4VideoPipelineRing *r)
{
    if (!r || !r->epoch) return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_READY) {
            r->slots[i].state = X4_PIPELINE_FREE; ++count;
        }
    r->ready -= count; r->occupied -= count; r->cancelled += count;
    return count;
}
bool x4_video_pipeline_ring_idle(const X4VideoPipelineRing *r)
{ return r && r->epoch && r->occupied == 0; }

int x4_video_pipeline_display_init(X4VideoPipelineDisplay *d, uint64_t epoch)
{
    if (!d || !epoch) return X4_PIPELINE_INVALID;
    memset(d, 0, sizeof(*d)); d->epoch = epoch; d->pending = d->screen = -1;
    return 0;
}
int x4_video_pipeline_display_occupancy(const X4VideoPipelineDisplay *d,
    X4VideoPipelineDisplayOccupancy *out)
{
    if(out)memset(out,0,sizeof(*out));
    if(!d || !d->epoch || !out)return X4_PIPELINE_INVALID;
    for(unsigned i=0;i<X4_PIPELINE_SCANOUTS;++i) {
        switch(d->slots[i].state) {
        case X4_SCANOUT_FREE:++out->free;break;
        case X4_SCANOUT_WRITING:++out->writing;break;
        case X4_SCANOUT_READY:++out->ready;break;
        case X4_SCANOUT_PENDING:++out->pending;break;
        case X4_SCANOUT_SCREEN:++out->screen;break;
        default:return X4_PIPELINE_STATE;
        }
    }
    out->in_flight=out->writing+out->ready+out->pending;
    out->occupied=out->in_flight+out->screen;
    return 0;
}
static int display_policy(X4VideoPipelineDisplay *d)
{
    X4VideoPipelineDisplayOccupancy o;
    if(x4_video_pipeline_display_occupancy(d,&o) || o.in_flight>1 || o.screen>1 ||
        d->pending < -1 || d->pending >= X4_PIPELINE_SCANOUTS ||
        d->screen < -1 || d->screen >= X4_PIPELINE_SCANOUTS ||
        (d->pending>=0 ? d->slots[d->pending].state!=X4_SCANOUT_PENDING : o.pending!=0) ||
        (d->screen>=0 ? d->slots[d->screen].state!=X4_SCANOUT_SCREEN : o.screen!=0)) {
        ++d->rejected;return X4_PIPELINE_STATE;
    }
    if(o.occupied>d->peak_occupied)d->peak_occupied=o.occupied;
    if(o.in_flight>d->peak_in_flight)d->peak_in_flight=o.in_flight;
    return 0;
}
static X4VideoPipelineLease scanout_lease(const X4VideoPipelineDisplay *d, unsigned slot)
{ return (X4VideoPipelineLease){slot, d->epoch, d->slots[slot].lease_generation}; }
static int scanout_check(X4VideoPipelineDisplay *d, X4VideoPipelineLease lease,
    X4VideoPipelineScanoutState state)
{
    if (!d || !d->epoch || lease.slot >= X4_PIPELINE_SCANOUTS) return X4_PIPELINE_INVALID;
    if (lease.epoch != d->epoch || !lease.generation ||
        lease.generation != d->slots[lease.slot].lease_generation) {
        ++d->rejected; return X4_PIPELINE_STALE;
    }
    if (d->slots[lease.slot].state != state) { ++d->rejected; return X4_PIPELINE_STATE; }
    return 0;
}
int x4_video_pipeline_acquire_scanout(X4VideoPipelineDisplay *d, X4VideoPipelineLease *out)
{
    clear_lease(out);
    if (!d || !d->epoch || !out) return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;
    for(unsigned i=0;i<X4_PIPELINE_SCANOUTS;++i)
        if(d->slots[i].state==X4_SCANOUT_WRITING || d->slots[i].state==X4_SCANOUT_READY ||
            d->slots[i].state==X4_SCANOUT_PENDING)return X4_PIPELINE_WAIT;
    if (d->next_generation == UINT64_MAX) return X4_PIPELINE_EXHAUSTED;
    for (unsigned i = 0; i < X4_PIPELINE_SCANOUTS; ++i)
        if (d->slots[i].state == X4_SCANOUT_FREE) {
            d->slots[i] = (X4VideoPipelineScanout){.state = X4_SCANOUT_WRITING,
                .lease_generation = ++d->next_generation};
            *out = scanout_lease(d, i); return display_policy(d);
        }
    return X4_PIPELINE_WAIT;
}
int x4_video_pipeline_finish_scanout(X4VideoPipelineDisplay *d, X4VideoPipelineLease lease,
    uint64_t output_id, uint64_t decode_end_us, uint64_t converted_us)
{
    int rc = scanout_check(d, lease, X4_SCANOUT_WRITING);
    if (rc) return rc;
    rc=display_policy(d);if(rc)return rc;
    if (!output_id || converted_us < decode_end_us) return X4_PIPELINE_INVALID;
    if (d->next_ready_order == UINT64_MAX) return X4_PIPELINE_EXHAUSTED;
    X4VideoPipelineScanout *s = &d->slots[lease.slot];
    s->state = X4_SCANOUT_READY; s->output_id = output_id; s->decode_end_us = decode_end_us;
    s->converted_us = converted_us; s->ready_order = ++d->next_ready_order;
    return display_policy(d);
}
int x4_video_pipeline_abort_scanout(X4VideoPipelineDisplay *d, X4VideoPipelineLease lease)
{
    int rc = scanout_check(d, lease, X4_SCANOUT_WRITING);
    if (rc) return rc;
    d->slots[lease.slot].state = X4_SCANOUT_FREE; return display_policy(d);
}
unsigned x4_video_pipeline_cancel_ready_scanout(X4VideoPipelineDisplay *d)
{
    if(!d || !d->epoch)return 0;
    unsigned count=0;
    for(unsigned i=0;i<X4_PIPELINE_SCANOUTS;++i)
        if(d->slots[i].state==X4_SCANOUT_READY){d->slots[i].state=X4_SCANOUT_FREE;++count;}
    (void)display_policy(d);return count;
}
int x4_video_pipeline_display_closed(X4VideoPipelineDisplay *d)
{
    if(!d || !d->epoch)return X4_PIPELINE_INVALID;
    for(unsigned i=0;i<X4_PIPELINE_SCANOUTS;++i)
        if(d->slots[i].state==X4_SCANOUT_WRITING)return X4_PIPELINE_STATE;
    for(unsigned i=0;i<X4_PIPELINE_SCANOUTS;++i)d->slots[i].state=X4_SCANOUT_FREE;
    d->pending=d->screen=-1;return display_policy(d);
}
int x4_video_pipeline_ready_scanout(X4VideoPipelineDisplay *d, X4VideoPipelineLease *out)
{
    clear_lease(out);
    if (!d || !d->epoch || !out) return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;
    if (d->pending >= 0) return X4_PIPELINE_WAIT;
    int slot = -1;
    for (unsigned i = 0; i < X4_PIPELINE_SCANOUTS; ++i)
        if (d->slots[i].state == X4_SCANOUT_READY &&
            (slot < 0 || d->slots[i].ready_order < d->slots[slot].ready_order)) slot = (int)i;
    if (slot < 0) return X4_PIPELINE_WAIT;
    *out = scanout_lease(d, (unsigned)slot); return 0;
}
int x4_video_pipeline_mark_submitted(X4VideoPipelineDisplay *d, X4VideoPipelineLease lease,
    uint64_t flip_arg, uint64_t submitted_us)
{
    int rc = scanout_check(d, lease, X4_SCANOUT_READY);
    if (rc) return rc;
    rc=display_policy(d);if(rc)return rc;
    if (d->pending >= 0) return X4_PIPELINE_STATE;
    if (!flip_arg || flip_arg <= d->last_submitted_arg ||
        submitted_us < d->slots[lease.slot].converted_us) return X4_PIPELINE_INVALID;
    X4VideoPipelineScanout *s = &d->slots[lease.slot];
    s->state = X4_SCANOUT_PENDING; s->flip_arg = flip_arg; s->submitted_us = submitted_us;
    d->pending = (int)lease.slot; d->last_submitted_arg = flip_arg; ++d->submitted;
    return display_policy(d);
}
int x4_video_pipeline_observe_flip(X4VideoPipelineDisplay *d, uint64_t flip_arg,
    uint64_t observed_us, X4VideoPipelineLease *presented)
{
    clear_lease(presented);
    if (!d || !d->epoch || !presented) return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;
    if (d->pending < 0 || d->slots[d->pending].flip_arg != flip_arg) return X4_PIPELINE_WAIT;
    X4VideoPipelineScanout *s = &d->slots[d->pending];
    if (observed_us < s->submitted_us) return X4_PIPELINE_INVALID;
    if (d->screen >= 0) d->slots[d->screen].state = X4_SCANOUT_FREE;
    d->screen = d->pending; d->pending = -1;
    s->state = X4_SCANOUT_SCREEN; s->observed_us = observed_us; ++d->observed;
    *presented = scanout_lease(d, (unsigned)d->screen); return display_policy(d);
}

int x4_video_pipeline_stripe(unsigned height, unsigned workers, unsigned index,
    X4VideoPipelineStripe *out)
{
    if (!out || !height || height > X4_PIPELINE_HEIGHT ||
        (workers != 2 && workers != 3) || index >= workers || height < workers)
        return X4_PIPELINE_INVALID;
    *out = (X4VideoPipelineStripe){height * index / workers, height * (index + 1) / workers};
    return 0;
}
static int validated_layout(const X4VideoNv12Layout *l, size_t source_bytes)
{
    if (!l || !((l->visible_width == 1280 && l->visible_height == 720 &&
            l->coded_width == 1280 && l->coded_height == 720 && l->storage_height == 720) ||
        (l->visible_width == 1920 && l->visible_height == 1080 &&
            l->coded_width == 1920 && l->coded_height == 1088 && l->storage_height == 1088)) ||
        l->pitch < l->coded_width || l->pitch > X4_VIDEO_MAX_PITCH || (l->pitch & 1))
        return X4_PIPELINE_INVALID;
    size_t y = (size_t)l->pitch * l->storage_height, uv = y / 2;
    size_t pixels = (size_t)l->visible_width * l->visible_height;
    if (l->y_bytes != y || l->uv_bytes != uv || l->uv_plane_offset != y ||
        l->total_nv12_bytes != y + uv || l->visible_y_offset != 0 ||
        l->visible_uv_offset != y || l->rgb_pixels != pixels ||
        l->rgb_bytes != pixels * sizeof(uint32_t) || source_bytes < y + uv)
        return X4_PIPELINE_INVALID;
    return 0;
}
static int validated_destination(const uint8_t *source, size_t source_bytes,
    uint32_t *dst, size_t bytes, unsigned width, unsigned height, unsigned pitch)
{
    if (!source || !dst || ((uintptr_t)dst & (sizeof(uint32_t) - 1)) ||
        pitch < width || pitch > X4_VIDEO_MAX_PITCH ||
        bytes < (size_t)pitch * height * sizeof(uint32_t)) return X4_PIPELINE_INVALID;
    uintptr_t a = (uintptr_t)source, b = (uintptr_t)dst;
    size_t written_span = (size_t)pitch * height * sizeof(uint32_t);
    if (source_bytes > UINTPTR_MAX - a || written_span > UINTPTR_MAX - b ||
        (a < b + written_span && b < a + source_bytes)) return X4_PIPELINE_INVALID;
    return 0;
}
static unsigned clamp_channel(int v)
{ return v < 0 ? 0u : v > 255 ? 255u : (unsigned)v; }
static uint32_t scalar_pixel(unsigned y, unsigned u, unsigned v)
{
    int c = (int)y - 16, d = (int)u - 128, e = (int)v - 128;
    if (c < 0) c = 0;
    unsigned r = clamp_channel((298*c + 409*e + 128) >> 8);
    unsigned g = clamp_channel((298*c - 100*d - 208*e + 128) >> 8);
    unsigned b = clamp_channel((298*c + 516*d + 128) >> 8);
    return 0xff000000u | b << 16 | g << 8 | r;
}
#if X4_PIPELINE_SSE2
/* Same BT.601 integer math, saturation and byte order as live_h264.c. Only
 * register-sized blocks exist; there is no intermediate NV12/RGB image. */
static void rgb_eight(__m128i y, __m128i u, __m128i v, __m128i *first, __m128i *second)
{
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi32(128);
    const __m128i rc = _mm_setr_epi16(298,409,298,409,298,409,298,409);
    const __m128i bc = _mm_setr_epi16(298,516,298,516,298,516,298,516);
    const __m128i gc = _mm_setr_epi16(-100,-208,-100,-208,-100,-208,-100,-208);
    const __m128i yc = _mm_set1_epi32(298), alpha = _mm_set1_epi8((char)-1);
    __m128i rl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,v),rc)),8);
    __m128i rh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,v),rc)),8);
    __m128i bl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,u),bc)),8);
    __m128i bh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,u),bc)),8);
    __m128i gl = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpacklo_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpacklo_epi16(u,v),gc))),8);
    __m128i gh = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpackhi_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpackhi_epi16(u,v),gc))),8);
    __m128i r = _mm_packus_epi16(_mm_packs_epi32(rl,rh),zero);
    __m128i g = _mm_packus_epi16(_mm_packs_epi32(gl,gh),zero);
    __m128i b = _mm_packus_epi16(_mm_packs_epi32(bl,bh),zero);
    __m128i rg = _mm_unpacklo_epi8(r,g), ba = _mm_unpacklo_epi8(b,alpha);
    *first = _mm_unpacklo_epi16(rg,ba); *second = _mm_unpackhi_epi16(rg,ba);
}
static void convert_row_sse2(uint32_t *dst, const uint8_t *y, const uint8_t *uv,
    unsigned source_width, bool scale_three_halves)
{
    const __m128i zero = _mm_setzero_si128(), ybias = _mm_set1_epi16(16), uvbias = _mm_set1_epi16(128);
    for (unsigned x = 0; x < source_width; x += 8) {
        __m128i luma = _mm_subs_epu16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(y+x)),zero),ybias);
        __m128i chroma = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(uv+x)),zero),uvbias);
        __m128i u = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(2,2,0,0)),_MM_SHUFFLE(2,2,0,0));
        __m128i v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(3,3,1,1)),_MM_SHUFFLE(3,3,1,1));
        __m128i first, second;
        rgb_eight(luma,u,v,&first,&second);
        if (scale_three_halves) {
            uint32_t *out = dst + x * 3 / 2;
            /* Eight source pixels become [0,0,1,2,2,3,4,4,5,6,6,7].
             * This is exactly floor(destination_x*2/3), using registers only. */
            _mm_storeu_si128((__m128i *)out,
                _mm_shuffle_epi32(first,_MM_SHUFFLE(2,1,0,0)));
            _mm_storeu_si128((__m128i *)(out+4),_mm_unpackhi_epi64(first,
                _mm_shuffle_epi32(second,_MM_SHUFFLE(0,0,0,0))));
            _mm_storeu_si128((__m128i *)(out+8),
                _mm_shuffle_epi32(second,_MM_SHUFFLE(3,2,2,1)));
        } else {
            _mm_storeu_si128((__m128i *)(dst+x),first);
            _mm_storeu_si128((__m128i *)(dst+x+4),second);
        }
    }
}
#endif
static void convert_rows(const X4VideoNv12Layout *l, const uint8_t *source,
    uint32_t *dst, unsigned dst_width, unsigned dst_height, unsigned dst_pitch,
    unsigned first_row, unsigned end_row)
{
    for (unsigned dy = first_row; dy < end_row; ++dy) {
        unsigned sy = dy * l->visible_height / dst_height;
        const uint8_t *y = source + (size_t)sy * l->pitch;
        const uint8_t *uv = source + l->uv_plane_offset + (size_t)(sy / 2) * l->pitch;
        uint32_t *row = dst + (size_t)dy * dst_pitch;
#if X4_PIPELINE_SSE2
        if (dst_width == l->visible_width ||
            (l->visible_width == 1280 && dst_width == 1920)) {
            convert_row_sse2(row,y,uv,l->visible_width,dst_width != l->visible_width);
            continue;
        }
#endif
        /* floor(x*source_width/destination_width), identical to the retained
         * live_media_draw nearest-neighbour map. No conversion of extra rows. */
        for (unsigned dx = 0; dx < dst_width; ++dx) {
            unsigned sx = dx * l->visible_width / dst_width, cx = sx & ~1u;
            row[dx] = scalar_pixel(y[sx], uv[cx], uv[cx+1]);
        }
    }
}
int x4_video_pipeline_convert_stripe(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes,
    unsigned dst_pitch, unsigned workers, unsigned index)
{
    X4VideoPipelineStripe stripe;
    int rc = x4_video_pipeline_stripe(X4_PIPELINE_HEIGHT,workers,index,&stripe);
    if (rc || validated_layout(l,source_bytes) || validated_destination(source,source_bytes,
            dst,dst_bytes,X4_PIPELINE_WIDTH,X4_PIPELINE_HEIGHT,dst_pitch)) return X4_PIPELINE_INVALID;
    convert_rows(l,source,dst,X4_PIPELINE_WIDTH,X4_PIPELINE_HEIGHT,dst_pitch,stripe.first_row,stripe.end_row);
#if defined(__SSE2__)
    /* Each converter drains its own WC writes before the caller publishes
     * completion. A later thread's fence cannot order this worker's stores. */
    _mm_sfence();
#endif
    return 0;
}
int x4_video_pipeline_convert_reference(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes)
{
    if (validated_layout(l,source_bytes) || validated_destination(source,source_bytes,
            dst,dst_bytes,l->visible_width,l->visible_height,l->visible_width)) return X4_PIPELINE_INVALID;
    convert_rows(l,source,dst,l->visible_width,l->visible_height,l->visible_width,0,l->visible_height);
    return 0;
}

/* New streaming conversion deliberately leaves the legacy/probe Q8 converter
 * above unchanged. Coefficients below are Q13, rounded from the inverse Y'CbCr
 * equations using Kr/Kb=0.299/0.114 (BT.601) or 0.2126/0.0722 (BT.709).
 * Limited range scales Y by 255/219 and chroma by 255/224; full range uses
 * unity Y and unscaled chroma. No source-range clamp precedes the matrix:
 * out-of-nominal Y/C values are clipped only after RGB reconstruction. */
typedef struct { int y, rv, gu, gv, bu, y_bias; } ColorCoefficients;
static ColorCoefficients color_coefficients(const X4VideoPipelineColor *color)
{
    bool hd = color->matrix_coefficients == 1;
    if (color->full_range)
        return hd ? (ColorCoefficients){8192,12901,-1535,-3835,15201,0} :
            (ColorCoefficients){8192,11485,-2819,-5850,14516,0};
    return hd ? (ColorCoefficients){9539,14686,-1747,-4366,17305,16} :
        (ColorCoefficients){9539,13075,-3209,-6660,16525,16};
}
static int validated_color_layout(const X4VideoNv12Layout *l, size_t bytes,
    unsigned *crop_x, unsigned *crop_y)
{
    if (!l || !crop_x || !crop_y || !l->visible_width || !l->visible_height ||
        !l->coded_width || !l->coded_height || l->coded_width > 1920 ||
        l->coded_height > 1088 || (l->coded_width & 1) || (l->coded_height & 1) ||
        l->storage_height < l->coded_height || l->storage_height > 1088 ||
        (l->storage_height & 1) || l->pitch < l->coded_width ||
        l->pitch > X4_VIDEO_MAX_PITCH || (l->pitch & 1)) return X4_PIPELINE_INVALID;
    size_t y = (size_t)l->pitch * l->storage_height, uv = y / 2;
    if (l->visible_y_offset >= y) return X4_PIPELINE_INVALID;
    unsigned cx = (unsigned)(l->visible_y_offset % l->pitch);
    unsigned cy = (unsigned)(l->visible_y_offset / l->pitch);
    if (cx >= l->coded_width || cy >= l->coded_height ||
        l->visible_width > l->coded_width - cx ||
        l->visible_height > l->coded_height - cy) return X4_PIPELINE_INVALID;
    size_t pixels = (size_t)l->visible_width * l->visible_height;
    if (l->y_bytes != y || l->uv_bytes != uv || l->uv_plane_offset != y ||
        l->total_nv12_bytes != y + uv || bytes < y + uv ||
        l->visible_uv_offset != y + (size_t)(cy / 2) * l->pitch + (cx & ~1u) ||
        l->rgb_pixels != pixels || l->rgb_bytes != pixels * sizeof(uint32_t))
        return X4_PIPELINE_INVALID;
    *crop_x = cx; *crop_y = cy;
    return 0;
}
static bool color_fast_geometry(const X4VideoNv12Layout *l,
    unsigned crop_x, unsigned crop_y)
{
    return !(crop_x & 1) && !(crop_y & 1) &&
        ((l->visible_width == 1280 && l->visible_height == 720 &&
          l->coded_width == 1280 && l->coded_height == 720 && l->storage_height == 720) ||
         (l->visible_width == 1920 && l->visible_height == 1080 &&
          l->coded_width == 1920 && l->coded_height == 1088 && l->storage_height == 1088));
}
int x4_video_pipeline_color_path(const X4VideoNv12Layout *l, size_t bytes,
    unsigned *path)
{
    unsigned cx, cy;
    if (!path || validated_color_layout(l,bytes,&cx,&cy)) return X4_PIPELINE_INVALID;
    *path = X4_PIPELINE_CONVERT_SCALAR;
#if X4_PIPELINE_SSE2
    if (color_fast_geometry(l,cx,cy)) *path = X4_PIPELINE_CONVERT_SSE2;
#else
    (void)color_fast_geometry;
#endif
    return 0;
}
static uint32_t color_scalar_pixel(unsigned y, unsigned u, unsigned v,
    const ColorCoefficients *m)
{
    int c = (int)y - m->y_bias, d = (int)u - 128, e = (int)v - 128;
    unsigned r = clamp_channel((m->y*c + m->rv*e + 4096) >> 13);
    unsigned g = clamp_channel((m->y*c + m->gu*d + m->gv*e + 4096) >> 13);
    unsigned b = clamp_channel((m->y*c + m->bu*d + 4096) >> 13);
    return 0xff000000u | b << 16 | g << 8 | r;
}
#if X4_PIPELINE_SSE2
static void color_rgb_eight(__m128i y, __m128i u, __m128i v,
    const ColorCoefficients *m, __m128i *first, __m128i *second)
{
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi32(4096);
    const __m128i rc = _mm_setr_epi16(m->y,m->rv,m->y,m->rv,m->y,m->rv,m->y,m->rv);
    const __m128i bc = _mm_setr_epi16(m->y,m->bu,m->y,m->bu,m->y,m->bu,m->y,m->bu);
    const __m128i gc = _mm_setr_epi16(m->gu,m->gv,m->gu,m->gv,m->gu,m->gv,m->gu,m->gv);
    const __m128i yc = _mm_set1_epi32(m->y), alpha = _mm_set1_epi8((char)-1);
    __m128i rl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,v),rc)),13);
    __m128i rh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,v),rc)),13);
    __m128i bl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,u),bc)),13);
    __m128i bh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,u),bc)),13);
    __m128i gl = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpacklo_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpacklo_epi16(u,v),gc))),13);
    __m128i gh = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpackhi_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpackhi_epi16(u,v),gc))),13);
    __m128i r = _mm_packus_epi16(_mm_packs_epi32(rl,rh),zero);
    __m128i g = _mm_packus_epi16(_mm_packs_epi32(gl,gh),zero);
    __m128i b = _mm_packus_epi16(_mm_packs_epi32(bl,bh),zero);
    __m128i rg = _mm_unpacklo_epi8(r,g), ba = _mm_unpacklo_epi8(b,alpha);
    *first = _mm_unpacklo_epi16(rg,ba); *second = _mm_unpackhi_epi16(rg,ba);
}
static void color_row_sse2(uint32_t *dst, const uint8_t *y, const uint8_t *uv,
    unsigned width, bool scale_three_halves, const ColorCoefficients *m)
{
    const __m128i zero = _mm_setzero_si128(), ybias = _mm_set1_epi16(m->y_bias);
    const __m128i uvbias = _mm_set1_epi16(128);
    for (unsigned x = 0; x < width; x += 8) {
        __m128i luma = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(y+x)),zero),ybias);
        __m128i chroma = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(uv+x)),zero),uvbias);
        __m128i u = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(2,2,0,0)),_MM_SHUFFLE(2,2,0,0));
        __m128i v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(3,3,1,1)),_MM_SHUFFLE(3,3,1,1));
        __m128i first, second; color_rgb_eight(luma,u,v,m,&first,&second);
        if (scale_three_halves) {
            uint32_t *out = dst + x * 3 / 2;
            _mm_storeu_si128((__m128i *)out,_mm_shuffle_epi32(first,_MM_SHUFFLE(2,1,0,0)));
            _mm_storeu_si128((__m128i *)(out+4),_mm_unpackhi_epi64(first,
                _mm_shuffle_epi32(second,_MM_SHUFFLE(0,0,0,0))));
            _mm_storeu_si128((__m128i *)(out+8),_mm_shuffle_epi32(second,_MM_SHUFFLE(3,2,2,1)));
        } else {
            _mm_storeu_si128((__m128i *)(dst+x),first);
            _mm_storeu_si128((__m128i *)(dst+x+4),second);
        }
    }
}
#endif
static void color_convert_rows(const X4VideoNv12Layout *l, const uint8_t *source,
    uint32_t *dst, unsigned width, unsigned height, unsigned pitch,
    unsigned first_row, unsigned end_row, unsigned crop_x, unsigned crop_y,
    const ColorCoefficients *m)
{
    for (unsigned dy = first_row; dy < end_row; ++dy) {
        unsigned sy = dy * l->visible_height / height;
        const uint8_t *y = source + l->visible_y_offset + (size_t)sy * l->pitch;
        const uint8_t *uv = source + l->uv_plane_offset +
            (size_t)((crop_y+sy)/2) * l->pitch + (crop_x & ~1u);
        uint32_t *row = dst + (size_t)dy * pitch;
#if X4_PIPELINE_SSE2
        if (color_fast_geometry(l,crop_x,crop_y) &&
            (width == l->visible_width || (l->visible_width == 1280 && width == 1920))) {
            color_row_sse2(row,y,uv,l->visible_width,width != l->visible_width,m);
            continue;
        }
#endif
        for (unsigned dx = 0; dx < width; ++dx) {
            unsigned sx = dx * l->visible_width / width;
            unsigned cx = ((crop_x + sx) & ~1u) - (crop_x & ~1u);
            row[dx] = color_scalar_pixel(y[sx],uv[cx],uv[cx+1],m);
        }
    }
}
int x4_video_pipeline_convert_stripe_color(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes,
    unsigned pitch, unsigned workers, unsigned index, const X4VideoPipelineColor *color)
{
    unsigned cx, cy; X4VideoPipelineStripe stripe;
    if (!color || x4_video_pipeline_stripe(1080,workers,index,&stripe) ||
        validated_color_layout(l,source_bytes,&cx,&cy) ||
        validated_destination(source,source_bytes,dst,dst_bytes,1920,1080,pitch))
        return X4_PIPELINE_INVALID;
    ColorCoefficients m = color_coefficients(color);
    color_convert_rows(l,source,dst,1920,1080,pitch,stripe.first_row,stripe.end_row,cx,cy,&m);
#if defined(__SSE2__)
    _mm_sfence();
#endif
    return 0;
}
int x4_video_pipeline_convert_reference_color(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes,
    const X4VideoPipelineColor *color)
{
    unsigned cx, cy;
    if (!color || validated_color_layout(l,source_bytes,&cx,&cy) ||
        validated_destination(source,source_bytes,dst,dst_bytes,
            l->visible_width,l->visible_height,l->visible_width)) return X4_PIPELINE_INVALID;
    ColorCoefficients m = color_coefficients(color);
    color_convert_rows(l,source,dst,l->visible_width,l->visible_height,
        l->visible_width,0,l->visible_height,cx,cy,&m);
    return 0;
}
