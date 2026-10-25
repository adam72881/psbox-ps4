/* SPDX-License-Identifier: GPL-3.0-or-later
 * ABR come Nexo adaptive_bitrate.h: si taglia su loss / local_dropped /
 * stale_ingress (RTP fermo), non sul solo receive rate (una scena statica
 * a 38 kbps non è congestione). Nessun taglio nei primi kAbrWarmupMs.
 * +250 kb/s dopo 20 s puliti. Floor 4 Mb/s a 720 e 6 a 1080.
 */
#ifndef PSBOX_ABR_PLAN_H
#define PSBOX_ABR_PLAN_H

#include <stdint.h>

enum AbrTier { ABR_720 = 0, ABR_1080 = 1, ABR_1440 = 2 };

static const uint32_t kAbrStepBps = 250000;
static const int kAbrCleanMs = 20000;
static const int kAbrStallMs = 1000;
static const int kAbrWarmupMs = 5000;
static const int kAbrPromoteCleanMs = 60000;

struct BitratePlan {
    int tier;
    uint32_t ceilingBps;
    uint32_t floorBps;
    uint32_t currentBps;
    unsigned long long cleanMs;
    unsigned long long aliveMs;
    unsigned int stallCuts;
    int lastChanged; /* 1 se l'ultimo sample ha mosso currentBps */
    const char* lastReason;
};

static uint32_t abrCeiling(int tier) {
    if (tier == ABR_720) return 8000000;
    if (tier == ABR_1440) return 12000000;
    return 12000000; /* 1080: tetto 12, partenza 10 */
}

static uint32_t abrFloor(int tier) {
    if (tier == ABR_720) return 4000000;
    return 6000000;
}

static BitratePlan bitrateBegin(int tier, uint32_t userCapBps) {
    BitratePlan p;
    uint32_t cap = abrCeiling(tier);
    if (userCapBps > 0 && userCapBps < cap) cap = userCapBps;
    if (cap < 1000000) cap = 1000000;
    p.tier = tier;
    p.ceilingBps = cap;
    p.floorBps = abrFloor(tier);
    if (p.floorBps > p.ceilingBps) p.floorBps = p.ceilingBps;
    /* 1080 parte a 10 Mb/s anche se il tetto SDP è 12. */
    p.currentBps = cap;
    if (tier == ABR_1080 && cap >= 10000000 && p.currentBps > 10000000) p.currentBps = 10000000;
    p.cleanMs = 0;
    p.aliveMs = 0;
    p.stallCuts = 0;
    p.lastChanged = 0;
    p.lastReason = "start";
    return p;
}

static int bitrateSdpKbps(const BitratePlan* p) {
    if (!p || p->ceilingBps < 1000) return 0;
    return (int)(p->ceilingBps / 1000u);
}

static uint32_t bitrateClamp(const BitratePlan* p, uint32_t v) {
    if (!p) return v;
    if (v < p->floorBps) v = p->floorBps;
    if (v > p->ceilingBps) v = p->ceilingBps;
    return v;
}

/* Taglio −40%. Ritorna 1 se current è cambiato. */
static int bitrateCut40(BitratePlan* p, const char* why) {
    uint32_t next;
    if (!p) return 0;
    next = p->currentBps - (p->currentBps * 2u) / 5u;
    next = bitrateClamp(p, next);
    p->cleanMs = 0;
    if (next == p->currentBps) {
        p->lastChanged = 0;
        return 0;
    }
    p->currentBps = next;
    p->lastChanged = 1;
    p->lastReason = why ? why : "cut";
    return 1;
}

/*
 * lossFraction = RR 0..255.
 * stallMs = ms senza pacchetto RTP video (stale_ingress).
 * localDropped = drop locali (coda piena / scarti) nell'intervallo.
 * recvBps è solo diagnostica: non guida il taglio (scena statica ≠ congestione).
 * Ritorna 1 se currentBps è cambiato.
 */
static int bitrateOnSample(BitratePlan* p, uint32_t recvBps, int lossFraction, int elapsedMs, int stallMs,
                           int localDropped) {
    uint32_t next;
    int changed = 0;
    (void)recvBps;
    if (!p) return 0;
    p->lastChanged = 0;
    if (elapsedMs < 0) elapsedMs = 0;
    if (stallMs < 0) stallMs = 0;
    if (localDropped < 0) localDropped = 0;

    int inWarmup = p->aliveMs < (unsigned long long)kAbrWarmupMs;
    p->aliveMs += (unsigned long long)elapsedMs;
    if (inWarmup) {
        p->lastReason = "warmup";
        return 0;
    }

    /* stale_ingress: nessun RTP per ≥ 1 s. */
    if (stallMs >= kAbrStallMs) {
        p->stallCuts++;
        if (bitrateCut40(p, "stale_ingress")) return 1;
        p->lastReason = "stall_floor";
        return 0;
    }

    /* Drop locali (coda / scarti decoder): congestione lato client. */
    if (localDropped > 0) {
        if (bitrateCut40(p, "local_dropped")) return 1;
        return 0;
    }

    if (lossFraction > 0) {
        next = p->currentBps - p->currentBps / 5u;
        next = bitrateClamp(p, next);
        p->cleanMs = 0;
        if (next != p->currentBps) {
            p->currentBps = next;
            p->lastChanged = 1;
            p->lastReason = "loss";
            return 1;
        }
        return 0;
    }

    p->cleanMs += (unsigned long long)elapsedMs;
    if (p->cleanMs >= (unsigned long long)kAbrCleanMs && p->currentBps < p->ceilingBps) {
        p->cleanMs = 0;
        next = bitrateClamp(p, p->currentBps + kAbrStepBps);
        if (next != p->currentBps) {
            p->currentBps = next;
            p->lastChanged = 1;
            p->lastReason = "raise";
            changed = 1;
        }
    }
    return changed;
}

/* Compat: solo RR loss, senza recv/stall. */
static void bitrateOnReport(BitratePlan* p, int lossFraction, int elapsedMs) {
    (void)bitrateOnSample(p, p ? p->currentBps : 0, lossFraction, elapsedMs, 0, 0);
}

/* REMB sempre (anche con TWCC): è il tetto che il server vede. */
static int bitrateSendRemb(const BitratePlan* p) {
    return p && p->currentBps > 0;
}

/* Dopo 60 s puliti a 720, se l'utente voleva 1080, si può promuovere. */
static int bitratePromoteDue(const BitratePlan* p) {
    return p && p->tier == ABR_720 && p->cleanMs >= (unsigned long long)kAbrPromoteCleanMs &&
           p->currentBps >= p->ceilingBps;
}

/* Due tagli da stall a 1080: cedere a 720. */
static int bitrateDemoteDue(const BitratePlan* p) {
    return p && p->tier >= ABR_1080 && p->stallCuts >= 2;
}

#endif
