/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_source.h"
/* Case owner reserves once before launching the probe video worker. Only that
 * worker hashes, sequentially, the index, clip and reference through this
 * buffer. Close follows successful worker/helper joins; it never races a hash.
 * Zero initialize before init; an absent or closed context fails safely. */
typedef struct { uint8_t *buffer; size_t capacity; } X4ProbeHashContext;
int x4_probe_hash_context_init(X4ProbeHashContext *);
void x4_probe_hash_context_close(X4ProbeHashContext *);
int x4_probe_hash_file(FILE *, uint64_t, uint8_t[32], const atomic_bool *, void *);
