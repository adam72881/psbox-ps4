/* SPDX-License-Identifier: GPL-3.0-or-later
 * Budget di riconnessione e stop che una nuova sessione non può cancellare.
 * La 01.48 chiudeva il DTLS a 8 s: ICE era su, l'handshake finiva al terzo
 * tentativo. requestStop azzerato da armNewSession faceva proseguire la
 * negoziazione durante la chiusura.
 */
#ifndef PSBOX_CONNECT_PLAN_H
#define PSBOX_CONNECT_PLAN_H

static const int kReconnectDtlsBudgetMs = 20000;
static const int kNegotiateSliceMs = 200;
static const int kNegotiateKeepaliveMs = 8000;
static const int kDtlsReinjectMs = 3000;
static const int kDtlsReinjectAgainMs = 8000;

struct StopFlags {
    int streamStop;
    int exitStop;
};

static int stopFlagsOn(const StopFlags* f) {
    return f && (f->streamStop || f->exitStop);
}

static void onRequestStop(StopFlags* f) {
    if (!f) return;
    f->streamStop = 1;
    f->exitStop = 1;
}

/* 0 se la chiusura è già chiesta: lo stop di stream resta attivo. */
static int onArmNewSession(StopFlags* f) {
    if (!f) return 0;
    if (f->exitStop) {
        f->streamStop = 1;
        return 0;
    }
    f->streamStop = 0;
    return 1;
}

/* Il handshake che diventa pronto a readyAtMs entra solo se readyAtMs < budget. */
static int dtlsHandshakeFits(int readyAtMs, int budgetMs) {
    return readyAtMs >= 0 && budgetMs > readyAtMs;
}

static int keepaliveDue(unsigned long long nowMs, unsigned long long lastMs) {
    if (!lastMs) return 1;
    return nowMs - lastMs >= (unsigned long long)kNegotiateKeepaliveMs;
}

#endif
