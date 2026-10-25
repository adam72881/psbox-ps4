/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_source.h"

enum { X4_PROBE_REFERENCE_MAX = 8, X4_PROBE_REFERENCE_MAE_MAX = 3,
    X4_PROBE_REFERENCE_CHANNEL_MAX = 16 };
typedef struct {
    uint64_t comparisons, references_read, pixels_compared, cancel_observations, errors;
    bool hash_verified;
} X4ProbeReferenceStats;
typedef struct {
    void *state;
    unsigned width, height, count;
    size_t frame_bytes;
    X4ProbeReferenceStats stats;
} X4ProbeReference;
typedef struct {
    unsigned best_reference_index, accepted_references, max_channel_delta;
    uint64_t sum_abs, samples, alpha_failures;
    double mean_abs;
    bool frame_identity_ambiguous;
} X4ProbeReferenceScore;

/* A generated, compiled descriptor supplies width/height/count and trusted
 * SHA. Files are decoded RGBA bytes concatenated without headers. One frame
 * buffer is reused for at most eight references; full-file hash is checked
 * before a comparison. This warmup validation must not enter steady timings.
 * Sole video owner calls every function; only the external cancellation atomic
 * may be written concurrently and must stay alive until close. */
int x4_probe_reference_open_verified(X4ProbeReference *, const char *local_path,
    unsigned width, unsigned height, unsigned count, const uint8_t expected_sha256[32],
    X4ProbeHashFile, void *hash_context, const atomic_bool *cancel);
/* Compare every visible RGB channel and require alpha=255 in both buffers.
 * A reference is acceptable iff mean absolute error <=3 and max channel error
 * <=16. Best score is descriptive even when none are acceptable. More than one
 * acceptable reference means frame identity is ambiguous; no output PTS is
 * invented. Layout identity requires the caller to compare distinct bounded
 * hypotheses and to find only one compatible layout across native outputs. */
int x4_probe_reference_match(X4ProbeReference *, const uint32_t *packed_rgba,
    size_t pixel_capacity, unsigned width, unsigned height, X4ProbeReferenceScore *out);
int x4_probe_reference_close(X4ProbeReference *);
