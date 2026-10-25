/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "product_metadata.h"
#include <stddef.h>
#include <stdint.h>

#define X4_CATALOG_ASSET_SLOTS 24u
#define X4_CATALOG_THUMBNAIL_BUDGET (24u * 1024u * 1024u)
typedef struct X4CatalogAssets X4CatalogAssets;
enum X4CatalogAssetState {
    X4_ASSET_PENDING, X4_ASSET_DOWNLOADING, X4_ASSET_READY,
    X4_ASSET_TEMPORARY_FAILURE, X4_ASSET_MISSING,
    X4_ASSET_LOADING = X4_ASSET_PENDING
};
enum X4AssetStage { X4_ASSET_QUEUE, X4_ASSET_METADATA, X4_ASSET_IMAGE,
    X4_ASSET_DECODE, X4_ASSET_PUBLISH, X4_ASSET_STAGE_COUNT };
enum X4AssetSource { X4_ASSET_RAM, X4_ASSET_DISK, X4_ASSET_NETWORK };
enum X4AssetFault { X4_ASSET_FAULT_NONE, X4_ASSET_FAULT_ALLOCATION, X4_ASSET_FAULT_CLEANUP };
typedef uint64_t (*X4AssetClock)(void *context);
typedef struct {
    const char *disk_root; /* NULL disables disk (offline fixtures). */
    X4AssetClock clock_us; /* NULL uses the platform monotonic clock. */
    void *clock_context;
} X4CatalogAssetsConfig;
typedef struct { uint64_t count, total_us, max_us; } X4AssetTiming;
#define X4_ASSET_EVENT_CAP 128u
typedef struct {
    uint64_t start_us, end_us, generation;
    char product_id[X4_PRODUCT_ID_CAP];
    enum X4AssetStage stage;
    enum X4AssetSource source;
} X4AssetEvent;
typedef struct {
    X4ProductMetadata metadata;
    X4CoverRaster surface[X4_COVER_SURFACES]; /* borrowed until release */
    enum X4CatalogAssetState state;
    uint64_t generation;
    unsigned slot;
} X4CatalogAssetView;
typedef struct {
    uint64_t requests, metadata_ready, covers_ready, failures, cancelled, stale_results;
    uint64_t revision;
    size_t thumbnail_bytes;
    unsigned occupied, pending, pinned;
    int busy, suspended, disk_enabled;
    uint64_t ram_hits, disk_metadata_hits, disk_image_hits;
    uint64_t metadata_network_requests, image_network_requests, batch_splits, partial_responses;
    uint64_t retries, deferred_stages, retry_suppressed, cleanup_failures, events_omitted;
    X4AssetTiming stage[X4_ASSET_STAGE_COUNT];
} X4CatalogAssetStats;

/* Main thread owns these APIs. One persistent native worker owns all public
 * HTTP and decoding. Starts suspended. create makes no HTTP request. */
X4CatalogAssets *x4_catalog_assets_create(void);
X4CatalogAssets *x4_catalog_assets_create_config(const X4CatalogAssetsConfig *config);
/* Reset exhausted temporary failures for demanded products. Retry-After
 * deadlines remain enforced. Permanent absence/invalid destinations do not retry. */
void x4_catalog_assets_retry(X4CatalogAssets *assets);
/* Call only at process shutdown; suspend first. The worker cleans up its own
 * HTTP resources before join. Navigation never calls this blocking join. */
void x4_catalog_assets_destroy(X4CatalogAssets *assets);
void x4_catalog_assets_set_generation(X4CatalogAssets *assets, uint64_t generation);
/* Replaces demand: selected first, visible catalog+Recents next, adjacent
 * pages last. Copies <=24 IDs; invalid/duplicate IDs are ignored. No HTTP. */
void x4_catalog_assets_request(X4CatalogAssets *assets, const char *const *product_ids, size_t count);
void x4_catalog_assets_suspend(X4CatalogAssets *assets);
/* Integration contract: resume only after the existing auth/network worker
 * is idle; suspend and await idle BEFORE starting auth/catalog/session work.
 * Thus existing global module/Net initialization never races. */
void x4_catalog_assets_resume(X4CatalogAssets *assets);
int x4_catalog_assets_idle(X4CatalogAssets *assets);
/* Enabled idle can retain public keep-alive resources. Foreground auth must
 * always suspend first, then await idle. A cleanup fault refuses that handoff. */
enum X4AssetFault x4_catalog_assets_fault(X4CatalogAssets *assets);
/* Nonblocking try-lock: missing/busy returns0; pins ownership before unlock.
 * Always release a successful acquire within the same draw. */
int x4_catalog_assets_acquire(X4CatalogAssets *assets, const char *product_id, X4CatalogAssetView *view);
void x4_catalog_assets_release(X4CatalogAssets *assets, X4CatalogAssetView *view);
/* Nonblocking snapshot of completed public metadata, not auth state. The
 * coordinator can retain names by product across LRU eviction. */
size_t x4_catalog_assets_copy_metadata(X4CatalogAssets *assets, X4ProductMetadata *out,
    size_t capacity, uint64_t *revision);
void x4_catalog_assets_stats(X4CatalogAssets *assets, X4CatalogAssetStats *out);
size_t x4_catalog_assets_events(X4CatalogAssets *assets, X4AssetEvent *out, size_t capacity);
