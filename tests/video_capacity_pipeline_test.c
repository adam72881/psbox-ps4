/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the real converter/output validator, not an extracted rewrite.
 * Native calls and workers are never invoked. Dead native functions are
 * discarded by the host linker; no mock decoder result proves native support. */
#include <assert.h>
#include <stdio.h>
#include "../src/video/live_h264.c"
static uint64_t checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static uint32_t scalar_pixel(int y, int u, int w)
{
    y -= 16; if (y < 0) y = 0; u -= 128; w -= 128;
    unsigned r = clamp((298*y + 409*w + 128) >> 8);
    unsigned g = clamp((298*y - 100*u - 208*w + 128) >> 8);
    unsigned b = clamp((298*y + 516*u + 128) >> 8);
    return 0xff000000u | (b << 16) | (g << 8) | r;
}
static void conversion(unsigned width, unsigned height, unsigned coded_width,
    unsigned storage_height, unsigned pitch, unsigned crop_x, unsigned crop_y, int extended)
{
    X4VideoCapacity capacity; CHECK(!x4_video_capacity_plan(extended, &capacity));
    size_t bytes = (size_t)pitch * storage_height * 3 / 2;
    uint8_t *nv12 = malloc(bytes); CHECK(nv12);
    uint32_t *pixels = malloc(((size_t)width * height + 16) * sizeof(*pixels)); CHECK(pixels);
    for (size_t i=0; i<bytes; ++i) nv12[i]=(uint8_t)((i*73u + (i>>7)*17u + 29u)&255u);
    for (size_t i=0; i<(size_t)width*height+16; ++i) pixels[i]=0xcafef00du;
    X4VideoNv12Request request={.visible_width=width,.visible_height=height,
        .coded_width=coded_width,.coded_height=storage_height,.storage_height=storage_height,
        .pitch=pitch,.crop_x=crop_x,.crop_y=crop_y,.format=X4_VIDEO_FORMAT_NV12,
        .mapping=X4_VIDEO_MAPPING_EXPLICIT_NV12,.source_bytes=bytes,.staging_bytes=bytes};
    X4VideoNv12Layout layout; CHECK(!x4_video_nv12_layout(&request,&capacity,(size_t)width*height,&layout));
    convert_nv12_sse2(pixels,nv12,&layout);
    for (unsigned y=0; y<height; ++y) for (unsigned x=0; x<width; ++x) {
        size_t uv=layout.visible_uv_offset+(size_t)(y/2)*pitch+(x&~1u);
        CHECK(pixels[(size_t)y*width+x]==scalar_pixel(nv12[layout.visible_y_offset+(size_t)y*pitch+x],nv12[uv],nv12[uv+1]));
    }
    for(size_t i=(size_t)width*height;i<(size_t)width*height+16;++i) CHECK(pixels[i]==0xcafef00du);
    free(pixels);free(nv12);
}
static void output_gate(void)
{
    VideoState state={0}; CHECK(!x4_video_capacity_plan(X4_VIDEO_CAPACITY_1080,&state.capacity));
    state.rgb_pixel_capacity=state.capacity.rgb_pixels;
    const size_t bytes=1280u*720u*3u/2u;
    void *native=aligned_alloc(16,bytes);CHECK(native);uint8_t *staging=malloc(bytes);CHECK(staging);
    state.output[0].p=native;state.output[0].size=bytes;state.staging=staging;state.staging_size=bytes;
    OrbisVideodec2OutputInfo o={.thisSize=sizeof(o),.isValid=true,.codecType=1,.frameWidth=1280,
        .frameHeight=720,.framePitch=1280,.pFrameBuffer=native,.frameBufferSize=bytes};
    unsigned reason;X4VideoNv12Layout layout;
    CHECK(validate_picture(&state,&o,&reason,&layout)==1);CHECK(layout.uv_plane_offset==1280u*720u);
    state.rgb_pixel_capacity=1280u*720u-1;CHECK(validate_picture(&state,&o,&reason,&layout)<0);state.rgb_pixel_capacity=state.capacity.rgb_pixels;
    --o.frameBufferSize;CHECK(validate_picture(&state,&o,&reason,&layout)<0);++o.frameBufferSize;
    --state.staging_size;CHECK(validate_picture(&state,&o,&reason,&layout)<0);++state.staging_size;
    o.frameWidth=1920;o.frameHeight=1080;o.framePitch=1920;CHECK(validate_picture(&state,&o,&reason,&layout)<0);
    o.frameHeight=1088;CHECK(validate_picture(&state,&o,&reason,&layout)<0);
    o.frameWidth=1280;o.frameHeight=720;o.framePitch=1280;o.codecType=2;CHECK(validate_picture(&state,&o,&reason,&layout)<0);o.codecType=1;
    o.framePitch=1281;CHECK(validate_picture(&state,&o,&reason,&layout)<0);o.framePitch=1280;
    o.pFrameBuffer=(uint8_t*)native+16;CHECK(validate_picture(&state,&o,&reason,&layout)<0);o.pFrameBuffer=native;
    o.isErrorFrame=true;CHECK(validate_picture(&state,&o,&reason,&layout)<0);o.isErrorFrame=false;
    o.isValid=false;CHECK(validate_picture(&state,&o,&reason,&layout)==0);
    free(staging);free(native);
}
int main(void)
{
    size_t total=99;
    X4VideoMemoryBlock legacy[]={{64u*1024u*1024u,16384},{16384,16384}};
    CHECK(x4_video_native_memory_total(legacy,2,&total)==X4_VIDEO_CAPACITY_MEMORY);
    CHECK(total==99);
    CHECK(!x4_video_native_memory_total_with_budget(legacy,2,X4_VIDEO_LEGACY_NATIVE_CEILING,&total));
    CHECK(total==64u*1024u*1024u+16384);
    CHECK(x4_video_native_memory_total_with_budget(legacy,2,0,&total)==X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_native_memory_total_with_budget(legacy,2,(size_t)X4_VIDEO_LEGACY_NATIVE_CEILING+1,&total)==X4_VIDEO_CAPACITY_ARGUMENT);
    X4VideoMemoryBlock maximum[]={{2u<<20,2u<<20},{2u<<20,2u<<20},
        {64u<<20,2u<<20},{64u<<20,2u<<20},{64u<<20,2u<<20},{64u<<20,2u<<20},
        {16u<<20,2u<<20},{16u<<20,2u<<20},{16u<<20,2u<<20},{16u<<20,2u<<20},{16384,16384}};
    CHECK(!x4_video_native_memory_total_with_budget(maximum,10,X4_VIDEO_LEGACY_NATIVE_CEILING,&total));
    CHECK(total==X4_VIDEO_LEGACY_NATIVE_CEILING);
    CHECK(x4_video_native_memory_total_with_budget(maximum,11,X4_VIDEO_LEGACY_NATIVE_CEILING,&total)==X4_VIDEO_CAPACITY_MEMORY);
    output_gate();
    conversion(1280,720,1280,720,1280,0,0,0);
    conversion(1280,720,1280,720,1536,0,0,0);
    conversion(18,4,32,8,64,2,2,0);
#if X4_VIDEO_CAPACITY_1080
    conversion(1920,1080,1920,1088,1920,0,0,1);
    conversion(1920,1080,1920,1088,2048,0,0,1);
#endif
    printf("PASS real converter/output gate flag=%u checks=%llu; no native API/performance claim\n",
        (unsigned)X4_VIDEO_CAPACITY_1080,(unsigned long long)checks);
    return 0;
}
