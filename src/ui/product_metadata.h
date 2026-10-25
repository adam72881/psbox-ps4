/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define X4_PRODUCT_ID_CAP 32
#define X4_PUBLIC_COVER_URL_CAP 512
typedef struct {
    char product_id[X4_PRODUCT_ID_CAP];
    char name[128], publisher[128], developer[128], category[64], description[512];
    char cover_url[X4_PUBLIC_COVER_URL_CAP];
} X4ProductMetadata;

int x4_product_id_valid(const char *id);
/* Parses only the matching validated ProductId from public Store metadata.
 * A name/image is not entitlement. Missing optional fields stay empty. */
int x4_product_metadata_parse(const char *json, size_t length, const char *product_id,
    X4ProductMetadata *out);
int x4_product_cover_url(const char *uri, char out[X4_PUBLIC_COVER_URL_CAP]);

enum X4CoverSurface { X4_COVER_GRID, X4_COVER_DETAIL, X4_COVER_RECENT, X4_COVER_SURFACES };
typedef struct { unsigned width, height; uint32_t *rgba; } X4CoverRaster;
typedef struct { X4CoverRaster surface[X4_COVER_SURFACES]; } X4CoverRasters;
/* Single image worker only. JPEG/PNG only; input<=2MiB, dimension<=2048,
 * aggregate decoder allocations<=16MiB. All three aspect-fit rasters are
 * produced once, and never resized during drawing. */
int x4_cover_decode(const unsigned char *encoded, size_t length, X4CoverRasters *out);
void x4_cover_rasters_free(X4CoverRasters *out);
