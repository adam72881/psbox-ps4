/* SPDX-License-Identifier: GPL-3.0-or-later
 * ABR reale: il tetto SDP è fisso; la stima REMB scende se il recv resta
 * sotto il 30% per 2 s, se il video è fermo oltre 1 s, o se il RR vede
 * perdite. +250 kb/s solo dopo 20 s puliti. Floor 4 Mb/s a 720 e 6 a 1080.
 * Politica riallineata a ciò che fanno XCloud4/Nexo (tetti bassi, taglio
 * sullo stall), reimplementata qui senza copiare il loro codice.
 */
#ifndef PSBOX_ABR_PLAN_H
#define PSBOX_ABR_PLAN_H

#include <stdint.h>

enum AbrTier { ABR_720 = 0, ABR_1080 = 1, ABR_1440 = 2 };

static const uint32_t kAbrStepBps = 250000;
static const int kAbrCleanMs = 20000;
static const int kAbrLowRecvMs = 2000;
static const int kAbrStallMs = 1000;
static const int kAbrPromoteCleanMs = 60000;

struct BitratePlan {
    int tier;
    uint32_t ceilingBps;
    uint32_t floorBps;
    uint32_t currentBps;
    unsigned long long cleanMs;
    unsigned long long lowRecvMs;
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
    p.lowRecvMs = 0;
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
    p->lowRecvMs = 0;
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
 * recvBps = throughput ricevuto nell'intervallo RR.
 * stallMs = ms senza pacchetto RTP video (0 se il video arriva).
 * lossFraction = RR 0..255.
 * Ritorna 1 se currentBps è cambiato (va loggato e va mandato REMB).
 */
static int bitrateOnSample(BitratePlan* p, uint32_t recvBps, int lossFraction, int elapsedMs, int stallMs) {
    uint32_t next;
    int changed = 0;
    if (!p) return 0;
    p->lastChanged = 0;
    if (elapsedMs < 0) elapsedMs = 0;
    if (stallMs < 0) stallMs = 0;

    if (stallMs >= kAbrStallMs) {
        p->stallCuts++;
        if (bitrateCut40(p, "stall")) return 1;
        /* Già al floor: conta comunque lo stall per un eventuale demote. */
        p->lastReason = "stall_floor";
        return 0;
    }

    /* recv < 30% del target per ≥ 2 s cumulativi. */
    if (p->currentBps > 0 && recvBps * 10u < p->currentBps * 3u) {
        p->lowRecvMs += (unsigned long long)elapsedMs;
        if (p->lowRecvMs >= (unsigned long long)kAbrLowRecvMs) {
            if (bitrateCut40(p, "recv_low")) return 1;
            return 0;
        }
    } else {
        p->lowRecvMs = 0;
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
    (void)bitrateOnSample(p, p ? p->currentBps : 0, lossFraction, elapsedMs, 0);
}

/* REMB sempre (anche con TWCC): è il tetto che il server vede. Senza REMB
 * la stima restava fissa a bitrate_ask mentre recv scendeva a 0 (01.51). */
static int bitrateSendRemb(const BitratePlan* p) {
    return p && p->currentBps > 0;
}

/* Dopo 60 s puliti a 720, se l'utente voleva 1080, si può promuovere. */
static int bitratePromoteDue(const BitratePlan* p) {
    return p && p->tier == ABR_720 && p->cleanMs >= (unsigned long long)kAbrPromoteCleanMs &&
           p->currentBps >= p->ceilingBps && p->lowRecvMs == 0;
}

/* Due tagli da stall a 1080: cedere a 720. */
static int bitrateDemoteDue(const BitratePlan* p) {
    return p && p->tier >= ABR_1080 && p->stallCuts >= 2;
}

#endif
