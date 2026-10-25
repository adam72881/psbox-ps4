/* SPDX-License-Identifier: GPL-3.0-only */
/* Arithmetic/layout tests only: no native decoder, network or PS4 execution. */
#include "video_capacity.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static void check(int condition, const char *expression, unsigned line)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL line %u: %s\n", line, expression);
        exit(1);
    }
}
#define CHECK(value) check(!!(value), #value, __LINE__)

static X4VideoNv12Request request(unsigned width, unsigned height, unsigned pitch)
{
    X4VideoNv12Request result = {
        .visible_width = width, .visible_height = height,
        .coded_width = width, .coded_height = height,
        .storage_height = height, .pitch = pitch,
        .format = X4_VIDEO_FORMAT_NV12, .mapping = X4_VIDEO_MAPPING_CURRENT_UNCROPPED,
        .source_bytes = (size_t)pitch * height * 3 / 2,
        .staging_bytes = (size_t)pitch * height * 3 / 2
    };
    return result;
}

static void arithmetic(void)
{
    size_t value = 77;
    CHECK(x4_video_size_add(1, 2, &value) == 0 && value == 3);
    CHECK(x4_video_size_add(SIZE_MAX, 0, &value) == 0 && value == SIZE_MAX);
    value = 77;
    CHECK(x4_video_size_add(SIZE_MAX, 1, &value) == X4_VIDEO_CAPACITY_OVERFLOW && value == 77);
    CHECK(x4_video_size_mul(0, SIZE_MAX, &value) == 0 && value == 0);
    CHECK(x4_video_size_mul(123, 4, &value) == 0 && value == 492);
    value = 77;
    CHECK(x4_video_size_mul(SIZE_MAX, 2, &value) == X4_VIDEO_CAPACITY_OVERFLOW && value == 77);
    CHECK(x4_video_size_align(16, 16, &value) == 0 && value == 16);
    CHECK(x4_video_size_align(17, 16, &value) == 0 && value == 32);
    CHECK(x4_video_size_align(0, 16, &value) == 0 && value == 0);
    CHECK(x4_video_size_align(17, 0, &value) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_size_align(17, 3, &value) == X4_VIDEO_CAPACITY_ARGUMENT);
    value = 77;
    CHECK(x4_video_size_align(SIZE_MAX, 16, &value) == X4_VIDEO_CAPACITY_OVERFLOW && value == 77);
    CHECK(x4_video_size_mul(1, 1, NULL) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_size_add(1, 1, NULL) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_size_align(1, 1, NULL) == X4_VIDEO_CAPACITY_ARGUMENT);
}

static X4VideoCapacity capacity_plan(void)
{
    X4VideoCapacity base, prepared;
    CHECK(x4_video_capacity_plan(0, &base) == 0);
    CHECK(base.visible_width == 1280 && base.visible_height == 720);
    CHECK(base.coded_width == 1280 && base.coded_height == 720 && base.storage_height == 720);
    CHECK(base.max_pitch == 8192 && base.rgb_pixels == 921600);
    CHECK(base.rgb_bytes == 3686400 && base.all_rgb_bytes == 11059200);
    CHECK(base.max_nv12_bytes == 8847360);
    CHECK(x4_video_capacity_plan(-1, &prepared) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_capacity_plan(2, &prepared) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_capacity_plan(0, NULL) == X4_VIDEO_CAPACITY_ARGUMENT);
#if X4_VIDEO_CAPACITY_1080
    CHECK(x4_video_capacity_plan(1, &prepared) == 0);
    CHECK(prepared.visible_width == 1920 && prepared.visible_height == 1080);
    CHECK(prepared.coded_width == 1920 && prepared.coded_height == 1088 && prepared.storage_height == 1088);
    CHECK(prepared.rgb_pixels == 2073600 && prepared.rgb_bytes == 8294400);
    CHECK(prepared.all_rgb_bytes == 24883200 && prepared.max_nv12_bytes == 13369344);
#else
    memset(&prepared, 0x5a, sizeof(prepared));
    CHECK(x4_video_capacity_plan(1, &prepared) == X4_VIDEO_CAPACITY_DISABLED);
    CHECK(prepared.visible_width == 0 && prepared.rgb_pixels == 0);
#endif
    return base;
}

static void baseline_layout(X4VideoCapacity base)
{
    X4VideoNv12Layout layout;
    const unsigned pitches[] = {1280, 1344, 1536, 2048, 4096, 8192};
    for (size_t i = 0; i < sizeof(pitches) / sizeof(*pitches); ++i) {
        X4VideoNv12Request r = request(1280, 720, pitches[i]);
        CHECK(x4_video_nv12_layout(&r, &base, base.rgb_pixels, &layout) == 0);
        CHECK(layout.y_bytes == (size_t)pitches[i] * 720);
        CHECK(layout.uv_bytes == (size_t)pitches[i] * 360);
        CHECK(layout.uv_plane_offset == layout.y_bytes && layout.visible_uv_offset == layout.y_bytes);
        CHECK(layout.total_nv12_bytes == r.source_bytes && layout.rgb_pixels == 921600);
        CHECK(layout.rgb_bytes == 3686400 && layout.visible_y_offset == 0);
    }
    X4VideoNv12Request r = request(1280, 720, 1280);
    CHECK(x4_video_nv12_layout(&r, &base, 921599, &layout) == X4_VIDEO_CAPACITY_DESTINATION);
    CHECK(layout.total_nv12_bytes == 0 && layout.rgb_bytes == 0);
    --r.source_bytes;
    CHECK(x4_video_nv12_layout(&r, &base, base.rgb_pixels, &layout) == X4_VIDEO_CAPACITY_STORAGE);
    ++r.source_bytes;
    --r.staging_bytes;
    CHECK(x4_video_nv12_layout(&r, &base, base.rgb_pixels, &layout) == X4_VIDEO_CAPACITY_STORAGE);
    r = request(640, 360, 640);
    CHECK(x4_video_nv12_layout(&r, &base, base.rgb_pixels, &layout) == 0);
    CHECK(layout.rgb_pixels == 230400 && layout.total_nv12_bytes == 345600);
    r = request(1920, 1080, 1920);
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_DIMENSIONS);
}

static void reject_cases(X4VideoCapacity base)
{
    X4VideoNv12Layout layout;
    const unsigned geometries[][8] = {
        {0,720,1280,720,720,1280,0,0},
        {1280,0,1280,720,720,1280,0,0},
        {1279,720,1280,720,720,1280,0,0},
        {1280,719,1280,720,720,1280,0,0},
        {1280,720,1279,720,720,1280,0,0},
        {1280,720,1280,719,720,1280,0,0},
        {1280,720,1280,720,719,1280,0,0},
        {1280,720,1280,720,720,1278,0,0},
        {1280,720,1280,720,720,1281,0,0},
        {1280,720,1280,720,720,8194,0,0},
        {1280,720,1280,720,722,1280,0,0},
        {1280,720,1282,720,720,1282,0,0},
        {1280,720,1280,720,720,1280,2,0},
        {1280,720,1280,720,720,1280,0,2},
        {1280,720,1280,720,720,1280,1,0},
        {1280,720,1280,720,720,1280,0,1},
        {1280,720,1280,720,720,1280,UINT_MAX,UINT_MAX}
    };
    for (size_t i = 0; i < sizeof(geometries) / sizeof(*geometries); ++i) {
        const unsigned *g = geometries[i];
        X4VideoNv12Request r = request(1280, 720, 1280);
        r.visible_width=g[0]; r.visible_height=g[1]; r.coded_width=g[2]; r.coded_height=g[3];
        r.storage_height=g[4]; r.pitch=g[5]; r.crop_x=g[6]; r.crop_y=g[7];
        r.mapping = X4_VIDEO_MAPPING_EXPLICIT_NV12;
        CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_DIMENSIONS);
        CHECK(layout.total_nv12_bytes == 0);
    }
    X4VideoNv12Request r = request(1280, 720, 1280);
    r.format = X4_VIDEO_FORMAT_UNKNOWN;
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_FORMAT);
    r.format = X4_VIDEO_FORMAT_I420;
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_FORMAT);
    r.format = X4_VIDEO_FORMAT_P010;
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_FORMAT);
    r.format = X4_VIDEO_FORMAT_NV12; r.mapping = (X4VideoMapping)0;
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_MAPPING);
    CHECK(x4_video_nv12_layout(NULL, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_nv12_layout(&r, NULL, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, NULL) == X4_VIDEO_CAPACITY_ARGUMENT);
    r = request(640, 360, 640); r.storage_height = 720;
    r.source_bytes = r.staging_bytes = 640 * 720 * 3 / 2;
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_MAPPING);
    r.mapping = X4_VIDEO_MAPPING_EXPLICIT_NV12;
    CHECK(x4_video_nv12_layout(&r, &base, SIZE_MAX, &layout) == 0);
    CHECK(layout.uv_plane_offset == 640 * 720);
}

static void cropped_layout(X4VideoCapacity base)
{
    /* Tiny explicit plane sentinel: cropped visible UV begins at storage Y
     * end, not visible Y end. No native ABI assumptions are exercised. */
    X4VideoNv12Request r = request(4, 2, 8);
    r.coded_height = r.storage_height = 4; r.crop_y = 2;
    r.mapping = X4_VIDEO_MAPPING_EXPLICIT_NV12;
    r.source_bytes = r.staging_bytes = 48;
    X4VideoNv12Layout layout;
    CHECK(x4_video_nv12_layout(&r, &base, 8, &layout) == 0);
    CHECK(layout.uv_plane_offset == 32 && layout.visible_y_offset == 16 && layout.visible_uv_offset == 40);
    unsigned char storage[48]; memset(storage, 0xaa, sizeof(storage));
    storage[layout.visible_y_offset] = 0x11; storage[layout.visible_uv_offset] = 0x22;
    CHECK(storage[16] == 0x11 && storage[40] == 0x22 && storage[8 * 2] != 0x22);
    r.mapping = X4_VIDEO_MAPPING_CURRENT_UNCROPPED;
    CHECK(x4_video_nv12_layout(&r, &base, 8, &layout) == X4_VIDEO_CAPACITY_MAPPING);
#if X4_VIDEO_CAPACITY_1080
    X4VideoCapacity prepared;
    CHECK(x4_video_capacity_plan(1, &prepared) == 0);
    const unsigned pitches[] = {1920, 2048, 4096, 8192};
    for (size_t i = 0; i < sizeof(pitches) / sizeof(*pitches); ++i) {
        r = request(1920, 1080, pitches[i]);
        r.coded_height = r.storage_height = 1088;
        r.mapping = X4_VIDEO_MAPPING_EXPLICIT_NV12;
        r.source_bytes = r.staging_bytes = (size_t)pitches[i] * 1088 * 3 / 2;
        CHECK(x4_video_nv12_layout(&r, &prepared, prepared.rgb_pixels, &layout) == 0);
        CHECK(layout.y_bytes == (size_t)pitches[i] * 1088 && layout.uv_bytes == (size_t)pitches[i] * 544);
        CHECK(layout.uv_plane_offset != (size_t)pitches[i] * 1080);
        CHECK(layout.total_nv12_bytes == r.source_bytes && layout.rgb_bytes == 8294400);
        r.mapping = X4_VIDEO_MAPPING_CURRENT_UNCROPPED;
        CHECK(x4_video_nv12_layout(&r, &prepared, prepared.rgb_pixels, &layout) == X4_VIDEO_CAPACITY_MAPPING);
        r.mapping = X4_VIDEO_MAPPING_EXPLICIT_NV12; r.crop_y = 8;
        CHECK(x4_video_nv12_layout(&r, &prepared, prepared.rgb_pixels, &layout) == 0);
        CHECK(layout.visible_y_offset == 8u * pitches[i] && layout.visible_uv_offset == layout.y_bytes + 4u * pitches[i]);
        r.source_bytes = (size_t)pitches[i] * 1080 * 3 / 2;
        CHECK(x4_video_nv12_layout(&r, &prepared, prepared.rgb_pixels, &layout) == X4_VIDEO_CAPACITY_STORAGE);
    }
#endif
}

static void overflow_layout(void)
{
    /* Fake hostile capacities ensure arithmetic remains checked even if a
     * caller supplies a capacity other than the trusted factory result. */
    const unsigned bound = UINT_MAX - 1;
    X4VideoCapacity capacity = { .visible_width=bound, .visible_height=bound,
        .coded_width=bound, .coded_height=bound, .storage_height=bound,
        .max_pitch=bound, .max_nv12_bytes=SIZE_MAX };
    X4VideoNv12Request r = { .visible_width=bound, .visible_height=bound,
        .coded_width=bound, .coded_height=bound, .storage_height=bound, .pitch=bound,
        .format=X4_VIDEO_FORMAT_NV12, .mapping=X4_VIDEO_MAPPING_EXPLICIT_NV12,
        .source_bytes=SIZE_MAX, .staging_bytes=SIZE_MAX };
    X4VideoNv12Layout layout;
    CHECK(x4_video_nv12_layout(&r, &capacity, SIZE_MAX, &layout) == X4_VIDEO_CAPACITY_OVERFLOW);
    CHECK(layout.total_nv12_bytes == 0 && layout.rgb_pixels == 0);
}

static void memory_budget(void)
{
    size_t total = 77;
    const X4VideoMemoryBlock normal[] = {
        {1u<<20, 0x4000}, {1u<<20, 0x4000}, {1u<<20, 0x4000}, {1u<<20, 0x4000},
        {2u<<20, 0x4000}, {2u<<20, 0x4000},
        {2u<<20, 0x4000}, {2u<<20, 0x4000}, {2u<<20, 0x4000}, {2u<<20, 0x4000}
    };
    CHECK(x4_video_native_memory_total(normal, 10, &total) == 0 && total == 16u * 1024 * 1024);
    X4VideoMemoryBlock block[] = {{X4_VIDEO_NATIVE_BUDGET, 0x4000}, {1, 0x4000}};
    CHECK(x4_video_native_memory_total(block, 1, &total) == 0 && total == X4_VIDEO_NATIVE_BUDGET);
    total = 77;
    CHECK(x4_video_native_memory_total(block, 2, &total) == X4_VIDEO_CAPACITY_MEMORY && total == 77);
    block[0].bytes = X4_VIDEO_NATIVE_BUDGET - 1;
    CHECK(x4_video_native_memory_total(block, 1, &total) == 0 && total == X4_VIDEO_NATIVE_BUDGET);
    block[0].bytes = X4_VIDEO_NATIVE_BUDGET + 1u;
    CHECK(x4_video_native_memory_total(block, 1, &total) == X4_VIDEO_CAPACITY_MEMORY);
    block[0].bytes = SIZE_MAX;
    CHECK(x4_video_native_memory_total(block, 1, &total) == X4_VIDEO_CAPACITY_OVERFLOW);
    block[0].bytes = 1; block[0].alignment = 0;
    CHECK(x4_video_native_memory_total(block, 1, &total) == 0 && total == X4_VIDEO_MIN_ALIGNMENT);
    block[0].alignment = 3;
    CHECK(x4_video_native_memory_total(block, 1, &total) == X4_VIDEO_CAPACITY_ARGUMENT);
    block[0].alignment = X4_VIDEO_MAX_ALIGNMENT * 2u;
    CHECK(x4_video_native_memory_total(block, 1, &total) == X4_VIDEO_CAPACITY_ARGUMENT);
    block[0].bytes = 1; block[0].alignment = X4_VIDEO_MAX_ALIGNMENT;
    CHECK(x4_video_native_memory_total(block, 1, &total) == 0 && total == X4_VIDEO_MAX_ALIGNMENT);
    block[0].bytes = 0; block[0].alignment = 3;
    CHECK(x4_video_native_memory_total(block, 1, &total) == 0 && total == 0);
    CHECK(x4_video_native_memory_total(NULL, 0, &total) == 0 && total == 0);
    CHECK(x4_video_native_memory_total(NULL, 1, &total) == X4_VIDEO_CAPACITY_ARGUMENT);
    CHECK(x4_video_native_memory_total(block, 1, NULL) == X4_VIDEO_CAPACITY_ARGUMENT);
    /* Per-component rounding matters; raw sums are not sufficient. */
    X4VideoMemoryBlock rounded[] = {{X4_VIDEO_NATIVE_BUDGET - X4_VIDEO_MIN_ALIGNMENT, 0}, {1, 0}, {1, 0}};
    CHECK(x4_video_native_memory_total(rounded, 3, &total) == X4_VIDEO_CAPACITY_MEMORY);
}

int main(void)
{
    arithmetic();
    X4VideoCapacity base = capacity_plan();
    baseline_layout(base); reject_cases(base); cropped_layout(base); overflow_layout(); memory_budget();
    printf("PASS video capacity flag=%d checks=%u; host arithmetic/layout only\n", X4_VIDEO_CAPACITY_1080, checks);
    return 0;
}
