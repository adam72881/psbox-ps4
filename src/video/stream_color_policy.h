/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Pure policy only: SPS matrix 1/5/6 always overrides the local preference.
 * AUTO uses BT.709 for unspecified video with at least 720 visible lines.
 * Other/unsupported matrix identifiers retain the prior BT.601 fallback.
 * Range is independent and is never changed by this decision. */
unsigned x4_stream_color_resolve(unsigned matrix, unsigned width,
    unsigned height, unsigned preference);
