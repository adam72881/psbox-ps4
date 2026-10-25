/* SPDX-License-Identifier: GPL-3.0-or-later
 * Dopo una perdita di riferimento non si decodifica un P-frame.
 * La coda piena, lo svuotamento del menu e un errore di decode entrano
 * tutti nello stesso stato: si scarta finché non arriva un IDR.
 */
#ifndef PSBOX_IDR_GATE_H
#define PSBOX_IDR_GATE_H

#include <stddef.h>
#include <stdint.h>

static const unsigned long long kIdrResetAfterMs = 5000;

enum IdrEvent {
    IDR_EVENT_NONE = 0,
    IDR_EVENT_QUEUE = 1,
    IDR_EVENT_MENU = 2,
    IDR_EVENT_DECODE = 3,
    IDR_EVENT_ERROR_FRAME = 4,
    IDR_EVENT_RESET = 5
};

struct IdrGate {
    int waiting;
    unsigned long long sinceMs;
    int resetArmed;
};

static void idrInit(IdrGate* g) {
    if (!g) return;
    g->waiting = 0;
    g->sinceMs = 0;
    g->resetArmed = 0;
}

static int auIsIdr(const uint8_t* d, size_t n) {
    size_t i;
    if (!d) return 0;
    for (i = 0; i + 4 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            if ((d[i + 4] & 0x1f) == 5) return 1;
        } else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if ((d[i + 3] & 0x1f) == 5) return 1;
        }
    }
    return 0;
}

static void idrOnEvent(IdrGate* g, int event, unsigned long long nowMs) {
    if (!g || event == IDR_EVENT_NONE) return;
    if (!g->waiting) g->sinceMs = nowMs;
    g->waiting = 1;
    g->resetArmed = 0;
}

/* 1 se questa AU può entrare nel decoder. */
static int idrAdmit(IdrGate* g, int isIdr) {
    if (!g || !g->waiting) return 1;
    if (!isIdr) return 0;
    g->waiting = 0;
    g->sinceMs = 0;
    g->resetArmed = 0;
    return 1;
}

/* 1 una sola volta quando l'attesa supera 5 s: il chiamante fa Reset. */
static int idrNeedsReset(IdrGate* g, unsigned long long nowMs) {
    if (!g || !g->waiting || g->resetArmed || !g->sinceMs) return 0;
    if (nowMs - g->sinceMs < kIdrResetAfterMs) return 0;
    g->resetArmed = 1;
    return 1;
}

#endif
