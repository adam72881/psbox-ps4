/* SPDX-License-Identifier: GPL-3.0-or-later
 * Una richiesta di keyframe ogni 500 ms, e non più di 10 in 5 s.
 * Il ritorno -1 è il tetto (conta come bloccata). 0 è solo «troppo presto».
 */
#ifndef PSBOX_KEYFRAME_LIMIT_H
#define PSBOX_KEYFRAME_LIMIT_H

#include <stdint.h>

static const unsigned long long kKfMinGapMs = 500;
static const unsigned long long kKfWindowMs = 5000;
static const int kKfMaxInWindow = 10;

struct KfLimit {
    unsigned long long lastMs;
    unsigned long long windowStart;
    int inWindow;
    int blockedNoted;
    int hasLast;
    int windowOpen;
};

static void kfInit(KfLimit* s) {
    if (!s) return;
    s->lastMs = 0;
    s->windowStart = 0;
    s->inWindow = 0;
    s->blockedNoted = 0;
    s->hasLast = 0;
    s->windowOpen = 0;
}

/* 1 invia, 0 troppo presto, -1 bloccata dal tetto dei 5 s.
 * Il primo istante può essere 0: non si usa lo zero come «mai inviato».
 * La finestra è (start, start+5s]: l'undicesima, a 5 s esatti, è bloccata. */
static int kfDecide(KfLimit* s, unsigned long long nowMs) {
    if (!s) return 0;
    if (s->hasLast && nowMs - s->lastMs < kKfMinGapMs) return 0;
    if (!s->windowOpen || nowMs - s->windowStart > kKfWindowMs) {
        s->windowStart = nowMs;
        s->windowOpen = 1;
        s->inWindow = 0;
        s->blockedNoted = 0;
    }
    if (s->inWindow >= kKfMaxInWindow) {
        if (s->blockedNoted) return 0;
        s->blockedNoted = 1;
        return -1;
    }
    s->lastMs = nowMs;
    s->hasLast = 1;
    s->inWindow++;
    return 1;
}

#endif
