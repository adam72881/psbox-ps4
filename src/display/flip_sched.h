/* SPDX-License-Identifier: GPL-3.0-or-later
 * Coda flip di VideoOut. La coda reale tiene circa 2 buffer. pickFree della
 * 01.48 guardava solo currentBuffer: con current == -1 ogni indice sembrava
 * libero, il decode a 1 ms riempiva la coda, SubmitFlip falliva e la porta
 * veniva chiusa. Da lì draw 0.0 e ogni frame in ritardo.
 */
#ifndef PSBOX_FLIP_SCHED_H
#define PSBOX_FLIP_SCHED_H

/* Un solo flip in volo: il decode non riempie la coda mentre il present è in ritardo. */
static const int kFlipQueueMax = 1;

static int flipIndexBusy(int index, int current, int pending, const int* recent, int nRecent) {
    int n;
    int i;
    if (pending > 0 && index == current) return 1;
    n = pending < nRecent ? pending : nRecent;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) {
        if (recent && recent[i] == index) return 1;
    }
    return 0;
}

/* -1 se la coda è già piena o ogni buffer è ancora in flip. */
static int flipPick(int bufCount, int start, int current, int pending, const int* recent, int nRecent) {
    int n;
    if (bufCount <= 0) return -1;
    if (pending >= kFlipQueueMax) return -1;
    if (start < 0) start = 0;
    for (n = 0; n < bufCount; n++) {
        int i = (start + n) % bufCount;
        if (!flipIndexBusy(i, current, pending, recent, nRecent)) return i;
    }
    return -1;
}

/* Un SubmitFlip rifiutato non chiude la porta. Il frame dopo riprova. */
static int flipErrorKillsPort(int rc) {
    (void)rc;
    return 0;
}

/* 1 se il flip è entrato in coda. portAlive resta 1. */
static int flipOnSubmit(int rc, int* portAlive) {
    if (rc == 0) return 1;
    if (portAlive && flipErrorKillsPort(rc)) *portAlive = 0;
    return 0;
}

#endif
