/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "catalog_browser.h"
#include "catalog_assets.h"

typedef struct {
    int connected, busy, pending;
    const char *notice;
} X4CatalogGuiStatus;

/* UI status text only; no retry or network side effect. Used by placeholders
 * and host checks so a temporary failure cannot look permanently missing. */
const char *x4_catalog_gui_asset_status(enum X4CatalogAssetState state);

/* The same 1920x1080 CPU renderer is used by the console and host captures.
 * Borrowed cache surfaces are pinned only for a draw; no resize, file I/O,
 * network, allocation or font rasterization occurs here. */
void x4_catalog_gui_draw(const X4CatalogSnapshot *catalog, const X4CatalogBrowser *browser,
    const X4RecentGames *recent, X4CatalogAssets *assets, const X4CatalogGuiStatus *status,
    uint32_t *pixels);
