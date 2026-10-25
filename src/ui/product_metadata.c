/* SPDX-License-Identifier: GPL-3.0-only */
#include "product_metadata.h"
#include "../auth/http_client.h"
#include "../auth/json.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int x4_product_id_valid(const char *id)
{
    if (!id) return 0;
    size_t n = 0;
    for (; n < X4_PRODUCT_ID_CAP && id[n]; ++n) {
        unsigned char c = (unsigned char)id[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return 0;
    }
    return n > 0 && n < X4_PRODUCT_ID_CAP;
}
int x4_product_cover_url(const char *uri, char out[X4_PUBLIC_COVER_URL_CAP])
{
    static const char host[] = "//store-images.s-microsoft.com/image/apps.";
    static const char suffix[] = "?w=512&q=80&format=jpg";
    if (!out) return 0;
    out[0] = 0;
    if (!uri) return 0;
    const char *base = uri;
    if (!strncmp(base, "https:", 6)) base += 6;
    if (strncmp(base, host, sizeof(host) - 1)) return 0;
    size_t n = 0;
    while (n < X4_PUBLIC_COVER_URL_CAP && base[n]) ++n;
    if (n >= X4_PUBLIC_COVER_URL_CAP || n + 6 + sizeof(suffix) > X4_PUBLIC_COVER_URL_CAP) return 0;
    for (size_t i = sizeof(host) - 1; i < n; ++i) {
        unsigned char c = (unsigned char)base[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    memcpy(out, "https:", 6);
    memcpy(out + 6, base, n);
    memcpy(out + 6 + n, suffix, sizeof(suffix));
    if (!x4_http_public_image_url(out)) { out[0] = 0; return 0; }
    return 1;
}
static void display(X4JsonSpan object, const char *key, char *out, size_t capacity)
{
    X4JsonSpan v;
    out[0] = 0;
    if (x4_json_member(object, key, &v) == 1) x4_json_display_text(v, out, capacity);
}
static int same_id(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return *a == *b;
}
static int image_rank(const char *purpose)
{
    if (!strcmp(purpose, "BrandedKeyArt")) return 5;
    if (!strcmp(purpose, "Poster")) return 4;
    if (!strcmp(purpose, "BoxArt")) return 3;
    if (!strcmp(purpose, "Tile")) return 2;
    if (!strcmp(purpose, "TitledHeroArt")) return 1;
    return 0;
}
int x4_product_metadata_parse(const char *json, size_t length, const char *id, X4ProductMetadata *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!x4_product_id_valid(id)) return -1;
    X4JsonSpan root, products, product, v, localized, first, properties;
    if (x4_json_parse(json, length, &root) || x4_json_member(root, "Products", &products) != 1) return -1;
    size_t cursor = 0;
    int found = 0, rc;
    while ((rc = x4_json_item(products, &cursor, &product)) == 1) {
        char product_id[X4_PRODUCT_ID_CAP];
        if (x4_json_member(product, "ProductId", &v) != 1 ||
            x4_json_token(v, product_id, sizeof(product_id)) || !x4_product_id_valid(product_id) ||
            !same_id(product_id, id)) continue;
        if (found) { memset(out, 0, sizeof(*out)); return -1; }
        found = 1;
        memcpy(out->product_id, id, strlen(id) + 1);
        if (x4_json_member(product, "Properties", &properties) == 1)
            display(properties, "Category", out->category, sizeof(out->category));
        size_t lc = 0;
        if (x4_json_member(product, "LocalizedProperties", &localized) != 1 ||
            x4_json_item(localized, &lc, &first) != 1) continue;
        display(first, "ProductTitle", out->name, sizeof(out->name));
        display(first, "PublisherName", out->publisher, sizeof(out->publisher));
        display(first, "DeveloperName", out->developer, sizeof(out->developer));
        display(first, "ShortDescription", out->description, sizeof(out->description));
        if (!out->description[0]) display(first, "ProductDescription", out->description, sizeof(out->description));
        X4JsonSpan images, image;
        if (x4_json_member(first, "Images", &images) != 1) continue;
        size_t ic = 0;
        int best = 0;
        while (x4_json_item(images, &ic, &image) == 1) {
            char purpose[64], uri[X4_PUBLIC_COVER_URL_CAP], url[X4_PUBLIC_COVER_URL_CAP];
            uint32_t width, height;
            if (x4_json_member(image, "ImagePurpose", &v) != 1 || x4_json_token(v, purpose, sizeof(purpose)) ||
                x4_json_member(image, "Uri", &v) != 1 || x4_json_token(v, uri, sizeof(uri)) ||
                x4_json_member(image, "Width", &v) != 1 || x4_json_uint32(v, &width) ||
                x4_json_member(image, "Height", &v) != 1 || x4_json_uint32(v, &height) ||
                !width || !height || width > 16384 || height > 16384 || !x4_product_cover_url(uri, url)) continue;
            int rank = image_rank(purpose);
            if (rank > best) { best = rank; memcpy(out->cover_url, url, strlen(url) + 1); }
        }
    }
    if (rc < 0) { memset(out, 0, sizeof(*out)); return -1; }
    return found ? 0 : -1;
}

/* The decoder is restricted to one cache worker. Budget includes temporary
 * buffers and allocation headers, not merely the final decoded pixels. */
static size_t decode_allocated;
#define X4_IMAGE_DECODE_BUDGET (16u * 1024u * 1024u)
typedef union { size_t size; max_align_t align; } Allocation;
static void *image_alloc(size_t n)
{
    if (n > X4_IMAGE_DECODE_BUDGET - sizeof(Allocation) ||
        decode_allocated > X4_IMAGE_DECODE_BUDGET - n - sizeof(Allocation)) return NULL;
    Allocation *a = malloc(sizeof(*a) + n);
    if (!a) return NULL;
    a->size = n + sizeof(*a); decode_allocated += a->size;
    return a + 1;
}
static void image_free(void *p)
{
    if (!p) return;
    Allocation *a = (Allocation *)p - 1;
    decode_allocated -= a->size; free(a);
}
static void *image_realloc(void *p, size_t n)
{
    if (!p) return image_alloc(n);
    Allocation *old = (Allocation *)p - 1;
    size_t size = old->size - sizeof(*old);
    void *fresh = image_alloc(n);
    if (!fresh) return NULL;
    memcpy(fresh, p, n < size ? n : size); image_free(p); return fresh;
}
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 2048
#define STBI_MALLOC(n) image_alloc(n)
#define STBI_FREE(p) image_free(p)
#define STBI_REALLOC(p,n) image_realloc(p,n)
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

static int raster(const unsigned char *source, unsigned sw, unsigned sh, unsigned w, unsigned h, X4CoverRaster *out)
{
    out->rgba = malloc((size_t)w * h * sizeof(uint32_t));
    if (!out->rgba) return -1;
    out->width = w; out->height = h;
    for (size_t i = 0; i < (size_t)w * h; ++i) out->rgba[i] = 0xff30261du;
    unsigned dw = w, dh = (unsigned)((uint64_t)sh * w / sw);
    if (dh > h) { dh = h; dw = (unsigned)((uint64_t)sw * h / sh); }
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    unsigned ox = (w - dw) / 2, oy = (h - dh) / 2;
    /* Once-only bilinear resampling; match x4_rgb/VideoOut 0xAABBGGRR,
     * with red in the low byte. The decoded source is byte RGBA. */
    for (unsigned y = 0; y < dh; ++y) {
        uint64_t fy = dh > 1 ? (uint64_t)y * (sh - 1) * 65536 / (dh - 1) : 0;
        unsigned sy = (unsigned)(fy >> 16), sy1 = sy + 1 < sh ? sy + 1 : sy;
        unsigned wy = (unsigned)(fy & 65535);
        for (unsigned x = 0; x < dw; ++x) {
            uint64_t fx = dw > 1 ? (uint64_t)x * (sw - 1) * 65536 / (dw - 1) : 0;
            unsigned sx = (unsigned)(fx >> 16), sx1 = sx + 1 < sw ? sx + 1 : sx;
            unsigned wx = (unsigned)(fx & 65535), channels[4];
            for (unsigned c = 0; c < 4; ++c) {
                uint64_t a = source[((size_t)sy * sw + sx) * 4 + c] * (65536u - wx) +
                    source[((size_t)sy * sw + sx1) * 4 + c] * wx;
                uint64_t b = source[((size_t)sy1 * sw + sx) * 4 + c] * (65536u - wx) +
                    source[((size_t)sy1 * sw + sx1) * 4 + c] * wx;
                channels[c] = (unsigned)((a * (65536u - wy) + b * wy) >> 32);
            }
            out->rgba[(size_t)(y + oy) * w + x + ox] = (channels[3] << 24) |
                (channels[2] << 16) | (channels[1] << 8) | channels[0];
        }
    }
    return 0;
}
void x4_cover_rasters_free(X4CoverRasters *out)
{
    if (!out) return;
    for (unsigned i = 0; i < X4_COVER_SURFACES; ++i) free(out->surface[i].rgba);
    memset(out, 0, sizeof(*out));
}
int x4_cover_decode(const unsigned char *encoded, size_t length, X4CoverRasters *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!encoded || !length || length > X4_HTTP_IMAGE_MAX || length > INT_MAX || decode_allocated) return -1;
    int w, h, channels;
    if (!stbi_info_from_memory(encoded, (int)length, &w, &h, &channels) || w <= 0 || h <= 0 || w > 2048 || h > 2048) return -1;
    unsigned char *pixels = stbi_load_from_memory(encoded, (int)length, &w, &h, &channels, 4);
    if (!pixels) return -1;
    int rc = raster(pixels, (unsigned)w, (unsigned)h, 320, 224, &out->surface[X4_COVER_GRID]) ||
        raster(pixels, (unsigned)w, (unsigned)h, 512, 288, &out->surface[X4_COVER_DETAIL]) ||
        raster(pixels, (unsigned)w, (unsigned)h, 96, 96, &out->surface[X4_COVER_RECENT]);
    stbi_image_free(pixels);
    if (rc) { x4_cover_rasters_free(out); return -1; }
    return 0;
}
