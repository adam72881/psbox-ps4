/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "xbox_catalog.h"
#include <stddef.h>
#define X4_RECENT_RESPONSE_MAX (256u * 1024u)
/* Portable fail-closed schema parser. Result IDs only, service order,
 * deduplicated. Does not read details entitlement or timestamps. */
int x4_xbox_recent_parse(const char *json, size_t length, X4RecentSnapshot *out);
