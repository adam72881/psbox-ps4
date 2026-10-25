/* SPDX-License-Identifier: GPL-3.0-or-later
 * Tetto dell'offerta SDP e stima che scende quando il RR vede perdite.
 * 720p parte a 8 Mb/s, 1080p a 12, 1440p a 15. Il tetto dell'utente, se
 * più basso, vince. Sotto il tetto si manda REMB; al tetto solo TWCC,
 * perché un REMB fisso a 25 Mb/s faceva litigare il GCC del server.
 */
#ifndef PSBOX_ABR_PLAN_H
#define PSBOX_ABR_PLAN_H

#include <stdint.h>

enum AbrTier { ABR_720 = 0, ABR_1080 = 1, ABR_1440 = 2 };

static const uint32_t kAbrFloorBps = 3000000;
static const uint32_t kAbrStepBps = 250000;
static const int kAbrCleanMs = 20000;

struct BitratePlan {
    uint32_t ceilingBps;
    uint32_t currentBps;
    unsigned long long cleanMs;
};

static uint32_t abrCeiling(int tier) {
    if (tier == ABR_720) return 8000000;
    if (tier == ABR_1440) return 15000000;
    return 12000000;
}

static BitratePlan bitrateBegin(int tier, uint32_t userCapBps) {
    BitratePlan p;
    uint32_t cap = abrCeiling(tier);
    if (userCapBps > 0 && userCapBps < cap) cap = userCapBps;
    if (cap < 1000000) cap = 1000000;
    p.ceilingBps = cap;
    p.currentBps = cap;
    p.cleanMs = 0;
    return p;
}

static int bitrateSdpKbps(const BitratePlan* p) {
    if (!p || p->ceilingBps < 1000) return 0;
    return (int)(p->ceilingBps / 1000u);
}

/* fraction è quella del RR (0..255). elapsedMs è l'intervallo del report. */
static void bitrateOnReport(BitratePlan* p, int lossFraction, int elapsedMs) {
    uint32_t next;
    if (!p) return;
    if (elapsedMs < 0) elapsedMs = 0;
    if (lossFraction > 0) {
        p->cleanMs = 0;
        next = p->currentBps - p->currentBps / 5;
        if (next < kAbrFloorBps) next = kAbrFloorBps;
        if (next > p->ceilingBps) next = p->ceilingBps;
        p->currentBps = next;
        return;
    }
    p->cleanMs += (unsigned long long)elapsedMs;
    if (p->cleanMs >= (unsigned long long)kAbrCleanMs && p->currentBps < p->ceilingBps) {
        p->cleanMs = 0;
        next = p->currentBps + kAbrStepBps;
        if (next > p->ceilingBps) next = p->ceilingBps;
        p->currentBps = next;
    }
}

/* REMB solo quando chiediamo meno del tetto. Al tetto il TWCC basta. */
static int bitrateSendRemb(const BitratePlan* p) {
    return p && p->currentBps < p->ceilingBps;
}

#endif
