/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_case.h"
#include "video_capacity.h"

enum {
    X4_PROBE_CONTRACT_ARGUMENT = -101,
    X4_PROBE_CONTRACT_IDENTITY = -102,
    X4_PROBE_CONTRACT_FORMAT = -103,
    X4_PROBE_CONTRACT_GEOMETRY = -104,
    X4_PROBE_CONTRACT_LEVEL = -105,
    X4_PROBE_CONTRACT_DPB = -106,
    X4_PROBE_CONTRACT_SCHEDULE = -107,
    X4_PROBE_CONTRACT_NATIVE = -108,
    X4_PROBE_CONTRACT_BOUNDS = -109
};

/* Checks the trusted fixture's internal consistency and allocation envelope.
 * This is neither native API capability enumeration nor a streaming policy.
 * Config fields are from verified SPS/tool evidence in generated descriptors. */
int x4_probe_case_validate(const X4ProbeCase *, const X4VideoCapacity *);

/* At most two bounded NV12 interpretations of the existing 48-byte ABI:
 * visible storage already cropped, or coded storage with the signaled crop.
 * Both are hypotheses until reference comparison validates a unique layout.
 * Native width/height and returned size do not establish plane offsets.
 * out is zeroed on entry/rejection; no source bytes are read or allocated. */
int x4_probe_layout_candidates(const X4ProbeCase *, unsigned native_width,
    unsigned native_height, unsigned pitch, size_t returned_bytes,
    size_t owned_bytes, size_t staging_bytes, size_t pixel_capacity,
    const X4VideoCapacity *, X4VideoNv12Layout out[2]);
