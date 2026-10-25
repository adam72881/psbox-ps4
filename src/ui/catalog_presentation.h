/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "catalog_browser.h"
#include "product_metadata.h"

enum { X4_PRESENTATION_HASH_SLOTS=8192, X4_PRESENTATION_DEMAND_MAX=24 };
typedef struct { char product_id[32],name[128];uint64_t seen_generation; } X4PresentationName;
/* Main-only public name overlay. <=704 KiB, no image pixels, no credentials
 * and no allocation/network operation. Covers remain in the separate LRU. */
typedef struct {
    X4PresentationName names[X4_CATALOG_MAX];
    uint16_t lookup[X4_PRESENTATION_HASH_SLOTS];
    unsigned count;
    uint64_t generation,last_revision,stale_results,rejected_metadata,capacity_rejected;
} X4CatalogPresentation;
void x4_catalog_presentation_init(X4CatalogPresentation *presentation);
/* Completed validated names survive refresh. Responses with an old
 * generation never enter this overlay, even if their product ID matches. */
void x4_catalog_presentation_set_generation(X4CatalogPresentation *presentation,uint64_t generation);
size_t x4_catalog_presentation_ingest(X4CatalogPresentation *presentation,uint64_t generation,
    uint64_t revision,const X4ProductMetadata *metadata,size_t count);
/* Modifies only presentation names in the main-owned catalog copy. The
 * provider IDs, access bits and auth worker's snapshot remain untouched.
 * Rebuild browser after true: its selected title ID remains the anchor. */
bool x4_catalog_presentation_apply_names(X4CatalogPresentation *presentation,X4CatalogSnapshot *catalog);
/* Priority: selected product, visible focused section, visible other section,
 * then one adjacent page in each section. Both visible pages use their actual
 * selection/page anchor even when that section is unfocused. Bound24,
 * product-ID dedup; no downloads performed here. */
size_t x4_catalog_presentation_demand(const X4CatalogPresentation *presentation,
    const X4CatalogBrowser *browser,const X4CatalogSnapshot *catalog,const X4RecentGames *recent,
    char product_ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP]);
