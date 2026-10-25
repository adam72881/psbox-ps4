/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t guard = UINT32_C(0xa531e26f);
static unsigned worst_error;
static uint64_t hash = UINT64_C(1469598103934665603);
static uint64_t pixel_checks;
static X4VideoNv12Layout layout(unsigned w, unsigned h, unsigned coded_w,
    unsigned coded_h, unsigned pitch, unsigned crop_x, unsigned crop_y)
{
    size_t y = (size_t)pitch * coded_h, pixels = (size_t)w*h;
    return (X4VideoNv12Layout){.visible_width=w,.visible_height=h,
        .coded_width=coded_w,.coded_height=coded_h,.storage_height=coded_h,.pitch=pitch,
        .y_bytes=y,.uv_bytes=y/2,.uv_plane_offset=y,.total_nv12_bytes=y+y/2,
        .visible_y_offset=(size_t)crop_y*pitch+crop_x,
        .visible_uv_offset=y+(size_t)(crop_y/2)*pitch+(crop_x&~1u),
        .rgb_pixels=pixels,.rgb_bytes=pixels*4};
}
static unsigned floating_channel(double x)
{ return x < 0 ? 0 : x > 255 ? 255 : (unsigned)floor(x+0.5); }
/* Independent floating-point inverse matrix, not rounded integer constants.
 * Kr/Kb values from ITU-R BT.601/BT.709; scaling follows digital 8-bit ranges. */
static uint32_t floating_reference(unsigned y, unsigned u, unsigned v,
    const X4VideoPipelineColor *color)
{
    double kr = color->matrix_coefficients == 1 ? 0.2126 : 0.299;
    double kb = color->matrix_coefficients == 1 ? 0.0722 : 0.114, kg = 1-kr-kb;
    double yy = color->full_range ? y : ((double)y-16)*255/219;
    double cb = ((double)u-128)*(color->full_range ? 1 : 255.0/224);
    double cr = ((double)v-128)*(color->full_range ? 1 : 255.0/224);
    unsigned r = floating_channel(yy + 2*(1-kr)*cr);
    unsigned b = floating_channel(yy + 2*(1-kb)*cb);
    unsigned g = floating_channel(yy - 2*kb*(1-kb)/kg*cb - 2*kr*(1-kr)/kg*cr);
    return UINT32_C(0xff000000) | r | g<<8 | b<<16;
}
static void compare_pixel(uint32_t actual, uint32_t expected)
{
    assert((actual >> 24) == 255);
    for (unsigned shift=0; shift<24; shift+=8) {
        int a = (actual>>shift)&255, b = (expected>>shift)&255;
        unsigned error = (unsigned)abs(a-b);
        if (error > worst_error) worst_error=error;
        assert(error <= 2);
    }
    hash = (hash ^ actual) * UINT64_C(1099511628211);
    ++pixel_checks;
}
static void fill_source(uint8_t *p, size_t size)
{
    uint32_t state = UINT32_C(0x9b843e17);
    for (size_t i=0;i<size;++i) {
        state ^= state<<13; state ^= state>>17; state ^= state<<5;
        p[i]=(uint8_t)state;
    }
    const uint8_t boundaries[]={0,1,15,16,17,127,128,129,234,235,236,254,255};
    for (size_t i=0;i<sizeof(boundaries)*100 && i<size;++i)
        p[i]=boundaries[i%sizeof(boundaries)];
}
typedef struct {
    X4VideoNv12Layout l; const uint8_t *src; uint32_t *dst; size_t bytes;
    X4VideoPipelineColor color; unsigned workers, index; int result;
} Job;
static void *convert_thread(void *arg)
{
    Job *j=arg;
    j->result=x4_video_pipeline_convert_stripe_color(&j->l,j->src,j->l.total_nv12_bytes,
        j->dst,j->bytes,1936,j->workers,j->index,&j->color);
    return NULL;
}
static void conversion_case(X4VideoNv12Layout l, unsigned matrix, bool full,
    unsigned workers, bool threaded)
{
    const size_t out_pixels=1936*1080, out_bytes=out_pixels*4, guard_count=16;
    uint8_t *src_raw=malloc(l.total_nv12_bytes+64), *src=src_raw+32;
    uint32_t *out_raw=malloc(out_bytes+guard_count*8), *out=out_raw+guard_count;
    uint32_t *ref=malloc(l.rgb_bytes);
    assert(src_raw && out_raw && ref);
    memset(src_raw,0x5a,l.total_nv12_bytes+64); fill_source(src,l.total_nv12_bytes);
    for (size_t i=0;i<out_pixels+guard_count*2;++i) out_raw[i]=guard;
    X4VideoPipelineColor color={matrix,full};
    unsigned path=UINT_MAX;
    assert(x4_video_pipeline_color_path(&l,l.total_nv12_bytes,&path)==0);
#if defined(__SSE2__) && !defined(X4_PIPELINE_SCALAR)
    bool fast = (l.visible_y_offset%l.pitch)%2==0 &&
        (l.visible_y_offset/l.pitch)%2==0 &&
        ((l.visible_width==1280 && l.visible_height==720 &&
          l.coded_width==1280 && l.coded_height==720 && l.storage_height==720) ||
         (l.visible_width==1920 && l.visible_height==1080 &&
          l.coded_width==1920 && l.coded_height==1088 && l.storage_height==1088));
    assert(path==(fast?X4_PIPELINE_CONVERT_SSE2:X4_PIPELINE_CONVERT_SCALAR));
#else
    assert(path==X4_PIPELINE_CONVERT_SCALAR);
#endif
    assert(x4_video_pipeline_convert_reference_color(&l,src,l.total_nv12_bytes,
        ref,l.rgb_bytes,&color)==0);
    if (threaded) {
        pthread_t threads[3]; Job jobs[3]; pthread_attr_t attr;
        assert(pthread_attr_init(&attr)==0);
        assert(pthread_attr_setstacksize(&attr,256*1024)==0);
        for (unsigned i=0;i<workers;++i) {
            jobs[i]=(Job){l,src,out,out_bytes,color,workers,i,-999};
            assert(pthread_create(&threads[i],&attr,convert_thread,&jobs[i])==0);
        }
        for (unsigned i=0;i<workers;++i) {
            assert(pthread_join(threads[i],NULL)==0 && jobs[i].result==0);
        }
        assert(pthread_attr_destroy(&attr)==0);
    } else for (unsigned i=0;i<workers;++i)
        assert(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,
            out,out_bytes,1936,workers,i,&color)==0);
    unsigned crop_x=(unsigned)(l.visible_y_offset%l.pitch);
    unsigned crop_y=(unsigned)(l.visible_y_offset/l.pitch);
    for (unsigned dy=0;dy<1080;++dy) {
        unsigned sy=dy*l.visible_height/1080, ay=crop_y+sy;
        for (unsigned dx=0;dx<1920;++dx) {
            unsigned sx=dx*l.visible_width/1920, ax=crop_x+sx;
            uint32_t reference=floating_reference(src[(size_t)ay*l.pitch+ax],
                src[l.uv_plane_offset+(size_t)(ay/2)*l.pitch+(ax&~1u)],
                src[l.uv_plane_offset+(size_t)(ay/2)*l.pitch+(ax&~1u)+1],&color);
            compare_pixel(out[(size_t)dy*1936+dx],reference);
            assert(out[(size_t)dy*1936+dx]==ref[(size_t)sy*l.visible_width+sx]);
        }
        for (unsigned x=1920;x<1936;++x) assert(out[(size_t)dy*1936+x]==guard);
    }
    for (size_t i=0;i<guard_count;++i)
        assert(out_raw[i]==guard && out[out_pixels+i]==guard);
    for (size_t i=0;i<32;++i)
        assert(src_raw[i]==0x5a && src[l.total_nv12_bytes+i]==0x5a);
    free(ref); free(out_raw); free(src_raw);
}
static void invalid_cases(void)
{
    X4VideoNv12Layout l=layout(1280,720,1280,720,1280,0,0), bad;
    X4VideoPipelineColor color={1,false}; unsigned path=UINT_MAX;
    size_t out_bytes=(size_t)1920*1080*4;
    uint8_t *src=calloc(1,l.total_nv12_bytes);
    uint32_t *dst=malloc(out_bytes);
    assert(src && dst); for(size_t i=0;i<out_bytes/4;++i)dst[i]=guard;
#define REJECT(expr) do { assert((expr)==X4_PIPELINE_INVALID); \
    for(size_t i=0;i<out_bytes/4;++i)assert(dst[i]==guard); } while(0)
#define RUN_BAD() REJECT(x4_video_pipeline_convert_stripe_color(&bad,src,l.total_nv12_bytes,dst,out_bytes,1920,2,0,&color))
    REJECT(x4_video_pipeline_color_path(NULL,l.total_nv12_bytes,&path)); assert(path==UINT_MAX);
    REJECT(x4_video_pipeline_color_path(&l,l.total_nv12_bytes,NULL));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes-1,dst,out_bytes,1920,2,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,dst,out_bytes-1,1920,2,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,dst,out_bytes,UINT_MAX,2,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,dst,out_bytes,1919,2,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,dst,out_bytes,1920,1,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,dst,out_bytes,1920,2,2,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,dst,out_bytes,1920,2,0,NULL));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,(uint32_t*)((uint8_t*)dst+1),out_bytes,1920,2,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,(uint32_t*)src,out_bytes,1920,2,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,(uint8_t*)(UINTPTR_MAX-4),l.total_nv12_bytes,dst,out_bytes,1920,2,0,&color));
    REJECT(x4_video_pipeline_convert_stripe_color(&l,src,l.total_nv12_bytes,(uint32_t*)(UINTPTR_MAX-3),out_bytes,1920,2,0,&color));
    REJECT(x4_video_pipeline_convert_reference_color(&l,src,l.total_nv12_bytes,dst,l.rgb_bytes-1,&color));
    bad=l;bad.coded_width=1922;RUN_BAD();
    bad=l;bad.coded_height=1090;RUN_BAD();
    bad=l;bad.visible_width=UINT_MAX;RUN_BAD();
    bad=l;bad.visible_height=UINT_MAX;RUN_BAD();
    bad=l;bad.visible_width=0;RUN_BAD();
    bad=l;bad.coded_width=1279;RUN_BAD();
    bad=l;bad.coded_height=719;RUN_BAD();
    bad=l;bad.storage_height=UINT_MAX;RUN_BAD();
    bad=l;bad.pitch=UINT_MAX;RUN_BAD();
    bad=l;bad.pitch=1281;RUN_BAD();
    bad=l;bad.pitch=1278;RUN_BAD();
    bad=l;bad.visible_y_offset=SIZE_MAX;RUN_BAD();
    bad=l;bad.visible_y_offset=2;RUN_BAD();
    bad=l;bad.visible_uv_offset++;RUN_BAD();
    bad=l;bad.y_bytes--;RUN_BAD();
    bad=l;bad.uv_bytes--;RUN_BAD();
    bad=l;bad.uv_plane_offset=SIZE_MAX;RUN_BAD();
    bad=l;bad.total_nv12_bytes=SIZE_MAX;RUN_BAD();
    bad=l;bad.rgb_pixels++;RUN_BAD();
    bad=l;bad.rgb_bytes++;RUN_BAD();
#undef RUN_BAD
#undef REJECT
    free(dst);free(src);
}
int main(void)
{
    const unsigned matrices[]={1,2,5,6,0,9};
    for(unsigned range=0;range<2;++range)for(size_t m=0;m<sizeof(matrices)/sizeof(matrices[0]);++m) {
        conversion_case(layout(1280,720,1280,720,1280,0,0),matrices[m],range!=0,2,true);
        conversion_case(layout(1920,1080,1920,1088,2048,0,0),matrices[m],range!=0,3,false);
    }
    /* General geometry and odd origins exercise absolute 4:2:0 chroma
     * selection. Last visible pixel is allowed to be odd; coded storage is even. */
    conversion_case(layout(957,535,1280,720,1408,1,3),1,false,2,false);
    conversion_case(layout(1278,718,1280,720,1280,2,2),1,true,3,false);
    /* Visible dimensions alone must not select the established SIMD path.
     * In particular the valid 720 crop-bottom=16 picture uses scalar when
     * native reports 1280x736 coded/storage; UV starts after all 736 rows. */
    conversion_case(layout(1280,720,1280,736,1280,0,0),1,false,2,false);
    conversion_case(layout(1920,1080,1920,1080,1920,0,0),1,true,2,false);
    conversion_case(layout(1920,1088,1920,1088,1920,0,0),1,false,2,false);
    conversion_case(layout(1,1,2,2,2,1,1),1,true,2,false);
    invalid_cases();
    printf("PASS color checks=%llu max_float_error=%u hash=%016llx\n",
        (unsigned long long)pixel_checks,worst_error,(unsigned long long)hash);
    puts("PASS 601/709 limited/full; exact scalar/SSE2 target hash, 720 scale, 1080 1:1, crop, guards, invalid no-write; no PS4 claim");
    return 0;
}
