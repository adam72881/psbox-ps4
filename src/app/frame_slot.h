/* SPDX-License-Identifier: GPL-3.0-or-later
 * Un solo frame in attesa di present: il più nuovo sostituisce il vecchio.
 * Il decode non aspetta il flip. Il flip in volo lo decide la coda VideoOut
 * (un submit alla volta).
 */
#ifndef PSBOX_FRAME_SLOT_H
#define PSBOX_FRAME_SLOT_H

#include <stdint.h>

struct FrameSlot {
    uint64_t seq;
    uint64_t replaced;
};

static void frameSlotInit(FrameSlot* s) {
    if (!s) return;
    s->seq = 0;
    s->replaced = 0;
}

static void frameReplace(FrameSlot* s, uint64_t seq) {
    if (!s) return;
    if (s->seq) s->replaced++;
    s->seq = seq;
}

static void frameShown(FrameSlot* s) {
    if (!s) return;
    s->seq = 0;
}

static int framePending(const FrameSlot* s) { return s && s->seq != 0; }

#endif
