// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Decisione di chiusura e sospensione, senza chiamate di sistema: il test
// offline e il thread di monitoraggio usano la stessa funzione.
#pragma once

#include <cstdint>

namespace xc::app {

// Layout ufficiale OrbisSystemServiceStatus (bool dopo eventNum). Prima si
// leggeva overlay come int32 → il bit background diventava "ov 256".
struct SysStatusFields {
    int32_t eventNum;
    uint8_t isSystemUiOverlaid;
    uint8_t isInBackgroundExecution;
    uint8_t isCpuMode7CpuNormal;
    uint8_t isGameLiveStreamingOnAir;
    uint8_t isOutOfVrPlayArea;
    uint8_t reserved[3];
};
static_assert(sizeof(SysStatusFields) == 12, "SceSystemServiceStatus SDK");

struct SysEventFields {
    int32_t eventType;
    uint8_t param[8192];
};
static_assert(sizeof(SysEventFields) == 8196, "SceSystemServiceEvent SDK");

enum class LifeCmd { None, ResumeRest, ResumeMenu, Pause, Close };

struct LifeState {
    bool seen = false;
    bool wasSus = false;
    bool wasMenu = false;
    bool exitArmed = false;  // pausa PS: teardown pronto se arriva la chiusura
    bool statusDead = false;
    bool receiveDead = false;
    // bg=1 continuo: dopo 300 ms in stream si rilasciano VideoOut/Audio
    // (stile HB-Store) così Chiudi passa; bg=0 → resume.
    uint64_t bgSinceMs = 0;
    bool streamReleased = false;
};

struct LifeSample {
    int statusRc = 0;
    int32_t eventNum = 0;
    int32_t overlay = 0;
    int32_t background = 0;
    int32_t cpuMode7 = 0;
    int32_t streaming = 0;
    int32_t cpu = 0;  // alias legacy: isCpuModeChanged / mode7
    int sysSus = 0;
    int appSus = 0;
    bool woke = false;
    bool haveEvent = false;
    int32_t eventType = 0;
    int receiveRc = 0;
    bool appMsg = false;
};

struct LifeResult {
    LifeCmd cmd = LifeCmd::None;
    bool logSuspend = false;
    bool logResume = false;
    bool logBackground = false;
    bool logClose = false;
    bool stopAll = false;
    bool stopReceive = false;
    bool releaseStream = false;  // bg>300ms: stop decode + release VO/Audio
    bool resumeStream = false;   // bg=0 dopo release
};

// Coda vuota: si continua a chiamare ReceiveEvent. 0x80A10004 = nessun
// evento (01.26 lo trattava come guasto e spegneva la coda → chiusura persa).
inline bool receiveEmpty(int rc) {
    auto u = static_cast<unsigned>(rc);
    return u == 0x80B20002u || u == 0x80B20004u || u == 0x8095000Eu || u == 0x80A10004u;
}

inline bool resumeEvent(int32_t t) {
    return t == 0x10000000 || t == 0x10000001;
}
inline bool shareEvent(int32_t t) { return t == static_cast<int32_t>(0x30000000); }

// Enum Orbis SystemService: non esiste un evento «terminate/close».
// 0x40010000* resta documentato per diagnostica; NON chiude l'app (01.39/01.40
// su FW 11.52: IsAppSuspended torna 0x80A10009 e uccideva il processo al boot).
inline bool terminateEvent(int32_t t) {
    auto u = static_cast<uint32_t>(t);
    if (u == 0x10000000u || u == 0x10000001u || u == 0x30000000u) return false;
    if (u == 0x40010000u || u == 0x40010001u || u == 0x40010002u) return true;
    return false;
}

// sceSystemServiceIsAppSuspended: solo un ritorno positivo = sospesa.
// 0 = attiva. Negativo = SCE error (es. 0x80A10009 = -2136932343): ignorare.
inline bool appSuspendedFromRc(int32_t rc) { return rc > 0; }

// Nessun API SystemService/AppMessaging è un «Chiudi applicazione» affidabile.
// Chiude solo Esci in-app o il kill del sistema (SIGKILL / soft-kill).
inline bool isCloseSignal(const LifeState& /*st*/, const LifeSample& /*s*/) { return false; }

inline LifeResult lifeStep(LifeState& st, const LifeSample& s) {
    LifeResult r;
    if (st.statusDead) return r;

    // GetStatus ko: spegne solo il polling status (niente dieNow).
    if (s.statusRc != 0) {
        st.statusDead = true;
        r.stopAll = true;
        return r;
    }

    if (s.woke) {
        r.cmd = LifeCmd::ResumeRest;
        r.logResume = true;
        st.wasSus = false;
        st.wasMenu = false;
        st.exitArmed = false;
        st.seen = true;
        return r;
    }

    bool suspended = s.sysSus || s.appSus;
    if (suspended) {
        if (!st.wasSus) r.logSuspend = true;
        st.wasSus = true;
        st.seen = true;
        if (!st.receiveDead && !s.haveEvent && s.receiveRc != 0 && !receiveEmpty(s.receiveRc)) {
            st.receiveDead = true;
            r.stopReceive = true;
        }
        return r;
    }
    if (st.wasSus) {
        st.wasSus = false;
        st.wasMenu = false;
        st.exitArmed = false;
        st.seen = true;
        r.cmd = LifeCmd::ResumeRest;
        r.logResume = true;
        return r;
    }

    // Overlay o background: menu PS / System UI (bool 0/1 dal GetStatus vero).
    bool inMenu = s.overlay != 0 || s.background != 0;
    if (st.seen && inMenu && !st.wasMenu) {
        r.cmd = LifeCmd::Pause;
        r.logBackground = true;
        st.exitArmed = true;
    } else if (st.seen && st.wasMenu && !inMenu) {
        r.cmd = LifeCmd::ResumeMenu;
        r.logResume = true;
        st.exitArmed = false;
        if (st.streamReleased) {
            r.resumeStream = true;
            st.streamReleased = false;
        }
        st.bgSinceMs = 0;
    }
    st.wasMenu = inMenu;
    st.seen = true;
    if (r.cmd == LifeCmd::None && !inMenu && s.haveEvent && resumeEvent(s.eventType)) {
        r.cmd = LifeCmd::ResumeRest;
        r.logResume = true;
        st.exitArmed = false;
    }
    if (!st.receiveDead && !s.haveEvent && s.receiveRc != 0 && !receiveEmpty(s.receiveRc)) {
        st.receiveDead = true;
        r.stopReceive = true;
    }
    return r;
}

}  // namespace xc::app
