/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>

#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#if X4_VIDEO_PROBE != 0 && X4_VIDEO_PROBE != 1
#error X4_VIDEO_PROBE must be 0 or 1
#endif

enum {
    X4_PROBE_INDEX_HEADER = 96, X4_PROBE_INDEX_RECORD = 24,
    X4_PROBE_SHA256_BYTES = 32, X4_PROBE_AU_MAX = 2 * 1024 * 1024,
    X4_PROBE_MAX_RECORDS = 90000, X4_PROBE_READ_CHUNK = 64 * 1024,
    X4_PROBE_PATH_MAX = 512
};
#define X4_PROBE_FILE_MAX UINT64_C(2147483648)
#define X4_PROBE_DURATION_MAX_US UINT64_C(900000000)

typedef enum {
    X4_PROBE_OK = 0, X4_PROBE_WAIT = 1, X4_PROBE_EOF = 2, X4_PROBE_CANCELLED = 3,
    X4_PROBE_ARGUMENT = -1, X4_PROBE_DISABLED = -2, X4_PROBE_STATE = -3,
    X4_PROBE_IO = -4, X4_PROBE_INDEX = -5, X4_PROBE_SIZE = -6,
    X4_PROBE_HASH = -7, X4_PROBE_CLOCK = -8, X4_PROBE_ANNEXB = -9,
    X4_PROBE_MEMORY = -10
} X4ProbeResult;
typedef enum { X4_PROBE_PACED = 0, X4_PROBE_MAX_SPEED = 1 } X4ProbeMode;

typedef struct {
    uint64_t file_bytes, records, duration_us;
    unsigned visible_width, visible_height, coded_width, coded_height;
    unsigned fps_num, fps_den;
    uint8_t source_sha256[X4_PROBE_SHA256_BYTES];
    uint8_t index_sha256[X4_PROBE_SHA256_BYTES];
} X4ProbeSourceInfo;

typedef struct {
    uint64_t ordinal, offset, bytes, pts_us, wait_us, late_us;
} X4ProbeAU;
typedef struct {
    uint64_t validated_records, aus_read, bytes_read, read_calls;
    uint64_t wait_observations, late_aus, max_late_us, cancel_observations, errors;
    bool hash_verified, index_hash_verified;
} X4ProbeSourceStats;

/* Single-owner FILEs. The current video worker owns them through close.
 * The new probe pipeline transfers exclusive reading to main after acquiring
 * source_ready, then back to the decoder for close via cleanup_allowed.
 * No concurrent FILE/state access is allowed. The optional cancellation flag
 * must remain alive through close; other threads may only set that flag. */
typedef struct { void *state; X4ProbeSourceInfo info; X4ProbeSourceStats stats; } X4ProbeSource;

/* Native binding uses the pinned library's actual SHA-256 API. This injected
 * callback hashes exactly expected_bytes using bounded reads and cancellation
 * checks, writes actual_sha256, and returns 0 / CANCELLED / negative failure.
 * It must not close FILE or log its contents. The source rewinds after it.
 * This interface does not invent a decoder or crypto capability API. */
typedef int (*X4ProbeHashFile)(FILE *, uint64_t expected_bytes,
    uint8_t actual_sha256[X4_PROBE_SHA256_BYTES], const atomic_bool *cancel, void *context);

/* X4AU1 index, little endian: 8-byte "X4AU1\0\0\0", u32 version=1,
 * u32 header_bytes=96; u64 file_bytes/records/duration_us; u32 pairs visible
 * width/height, coded width/height, fps numerator/denominator; 32-byte source
 * SHA256. Then records of u64 offset/bytes/relative_pts_us. Records cover the
 * AnnexB file contiguously and PTS starts at zero and strictly increases.
 * The caller's SHA is a trusted compiled sample descriptor, not the filename
 * or mutable index alone. Generator/offline tools establish AU/SPS semantics;
 * this module verifies identity and safe indexing, not compressed correctness. */
int x4_probe_source_open_verified(X4ProbeSource *, const char *annexb_path,
    const char *index_path, const uint8_t expected_sha256[X4_PROBE_SHA256_BYTES],
    X4ProbeHashFile, void *hash_context, const atomic_bool *cancel);
/* Native probe uses this stronger entry point. The compiled index SHA binds
 * AU boundaries, timestamps and metadata in addition to clip identity. Both
 * hashes are verified on the very FILEs retained by the sole source owner.
 * The preceding API remains for isolated reader fixtures, not native cases. */
int x4_probe_source_open_fully_verified(X4ProbeSource *, const char *annexb_path,
    const char *index_path, const uint8_t expected_clip_sha256[32],
    const uint8_t expected_index_sha256[32], X4ProbeHashFile, void *hash_context,
    const atomic_bool *cancel);
int x4_probe_source_begin(X4ProbeSource *, uint64_t origin_now_us, X4ProbeMode);
/* WAIT does not consume/drop an AU. The absolute origin + PTS deadline is
 * never moved by backlog. MAX_SPEED still reads every AU and relies on the
 * production worker/mailbox backpressure; it is not a second renderer. */
int x4_probe_source_next(X4ProbeSource *, uint64_t now_us, uint8_t *destination,
    size_t destination_capacity, X4ProbeAU *out);
void x4_probe_source_cancel(X4ProbeSource *); /* owner only */
int x4_probe_source_close(X4ProbeSource *); /* owner only; stats remain readable */
/* Arithmetic scheduling is pure and testable even with the probe disabled.
 * No sleeping, frame dropping or decoder/output PTS association occurs here. */
int x4_probe_schedule(uint64_t origin_us, uint64_t pts_us, uint64_t now_us,
    X4ProbeMode, uint64_t *wait_us, uint64_t *late_us);
