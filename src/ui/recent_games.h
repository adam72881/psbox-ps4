/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../auth/xbox_catalog.h"

enum X4RecentState { X4_RECENT_IDLE, X4_RECENT_LOADING, X4_RECENT_READY, X4_RECENT_ERROR };
typedef struct { char id[96], product_id[32], name[128]; } X4RecentGame;

/* Temporary presentation of provider MRU, in provider order. No local
 * persistence or authentication data. Main-thread owner; account/request
 * epochs are opaque coordinator identities, not user IDs or credentials. */
typedef struct X4RecentGames {
    X4RecentGame entries[X4_RECENT_MAX];
    unsigned count;
    enum X4RecentState state;
    int error, http_status;
    uint64_t account_generation, generation, stale_results;
} X4RecentGames;

void x4_recent_init(X4RecentGames *history);
void x4_recent_begin(X4RecentGames *history, uint64_t account_generation, uint64_t request_generation);
/* IDs are titleIds from the server's results[].titleId, never product IDs.
 * Preserve first occurrence/order; reject malformed/oversized results.
 * Old-account/old-request responses cannot mutate the current selection. */
bool x4_recent_publish(X4RecentGames *history, uint64_t account_generation,
    uint64_t request_generation, const char ids[][96], unsigned count,
    int error, int http_status, const X4CatalogSnapshot *catalog);
void x4_recent_refresh_catalog(X4RecentGames *history, const X4CatalogSnapshot *catalog);
unsigned x4_recent_count(const X4RecentGames *history);
const X4RecentGame *x4_recent_get(const X4RecentGames *history, unsigned index);
/* A server-history item is not proof of access. Resolve against the current
 * validated READY catalog; missing IDs fail closed. */
bool x4_recent_catalog_index(const X4RecentGame *game, const X4CatalogSnapshot *catalog,
    unsigned *source_index);
bool x4_recent_title_id_valid(const char *id, size_t capacity);
bool x4_recent_display_name_valid(const char *name);
