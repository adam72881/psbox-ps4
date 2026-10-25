// g++ -std=c++20 -I src -o /tmp/lifecycle_test tests/lifecycle_test.cpp
#include "app/lifecycle_decide.h"

#include <cstdio>
#include <ctime>

static int g_fail;

static void expect(const char* name, bool cond) {
    if (cond) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    ++g_fail;
}

int main() {
    using xc::app::LifeCmd;
    using xc::app::LifeSample;
    using xc::app::LifeState;
    using xc::app::appSuspendedFromRc;
    using xc::app::isCloseSignal;
    using xc::app::lifeStep;
    using xc::app::receiveEmpty;
    using xc::app::terminateEvent;

    expect("status 12", sizeof(xc::app::SysStatusFields) == 12);
    expect("evento 8196", sizeof(xc::app::SysEventFields) == 8196);
    expect("coda vuota B20004", receiveEmpty(static_cast<int>(0x80B20004)));
    expect("coda vuota A10004", receiveEmpty(static_cast<int>(0x80A10004)));
    expect("zero non e vuoto", !receiveEmpty(0));
    expect("receive ko non vuoto", !receiveEmpty(static_cast<int>(0x80B20001)));
    expect("terminate 0x40010000 documentato", terminateEvent(0x40010000));
    expect("JoinMatch non terminate", !terminateEvent(0x10000010));
    expect("resume non terminate", !terminateEvent(0x10000000));

    // Bug 01.39/01.40: IsAppSuspended → 0x80A10009 (-2136932343) al boot.
    expect("rc errore non sospeso", !appSuspendedFromRc(static_cast<int32_t>(0x80A10009)));
    expect("rc -2136932343 non sospeso", !appSuspendedFromRc(-2136932343));
    expect("rc 0 attivo", !appSuspendedFromRc(0));
    expect("rc 1 sospeso", appSuspendedFromRc(1));
    expect("rc positivo sospeso", appSuspendedFromRc(42));

    LifeState st;
    LifeSample s;
    auto r = lifeStep(st, s);
    expect("primo giro fermo", r.cmd == LifeCmd::None && st.seen && !r.stopAll);

    s.background = 1;
    r = lifeStep(st, s);
    expect("background in pausa", r.cmd == LifeCmd::Pause && r.logBackground && st.exitArmed);
    s.background = 0;
    r = lifeStep(st, s);
    expect("ritorno dal menu", r.cmd == LifeCmd::ResumeMenu && r.logResume && !st.exitArmed);

    // Soft-release in bg: al ritorno si chiede resumeStream (VideoOut).
    st = {};
    s = {};
    lifeStep(st, s);
    s.background = 1;
    r = lifeStep(st, s);
    expect("bg arma pausa", r.cmd == LifeCmd::Pause);
    st.streamReleased = true;
    s.background = 0;
    r = lifeStep(st, s);
    expect("ritorno dopo soft-release", r.cmd == LifeCmd::ResumeMenu && r.resumeStream && !st.streamReleased);

    st = {};
    s = {};
    s.background = 1;
    s.sysSus = 1;
    r = lifeStep(st, s);
    expect("sospensione non chiude", r.cmd == LifeCmd::None && r.logSuspend && st.wasSus);
    s.sysSus = 0;
    s.background = 1;
    r = lifeStep(st, s);
    expect("uscita dal rest non chiude subito", r.cmd == LifeCmd::ResumeRest && r.logResume);
    r = lifeStep(st, s);
    expect("giro dopo, background in pausa", r.cmd == LifeCmd::Pause && r.logBackground);

    st = {};
    s = {};
    lifeStep(st, s);
    s.woke = true;
    s.background = 1;
    r = lifeStep(st, s);
    expect("salto di tempo riprende", r.cmd == LifeCmd::ResumeRest && !st.wasSus);

    // Nessun trigger di sistema chiude (01.41).
    st = {};
    s = {};
    lifeStep(st, s);
    s.woke = true;
    s.haveEvent = true;
    s.eventType = 0x40010000;
    r = lifeStep(st, s);
    expect("terminate documentato non chiude", r.cmd != LifeCmd::Close && !isCloseSignal(st, s));

    st = {};
    s = {};
    s.statusRc = static_cast<int>(0x80B20001);
    r = lifeStep(st, s);
    expect("getstatus ko spegne poll", r.stopAll && st.statusDead && r.cmd == LifeCmd::None);
    s.statusRc = 0;
    s.background = 1;
    r = lifeStep(st, s);
    expect("spento resta spento", r.cmd == LifeCmd::None && !r.stopAll);

    st = {};
    s = {};
    lifeStep(st, s);
    s.receiveRc = static_cast<int>(0x80B20001);
    r = lifeStep(st, s);
    expect("receive ko spegne solo gli eventi", r.stopReceive && st.receiveDead && !st.statusDead);
    s.receiveRc = 0;
    s.background = 1;
    r = lifeStep(st, s);
    expect("background con receive ko non chiude", r.cmd == LifeCmd::Pause && r.logBackground);
    s.background = 0;
    s.overlay = 1;
    r = lifeStep(st, s);
    expect("overlay 1 resta in menu", r.cmd == LifeCmd::None && st.wasMenu);

    st = {};
    s = {};
    lifeStep(st, s);
    s.receiveRc = static_cast<int>(0x80B20004);
    r = lifeStep(st, s);
    expect("coda vuota non spegne", !r.stopReceive && !st.receiveDead);

    st = {};
    s = {};
    lifeStep(st, s);
    s.receiveRc = static_cast<int>(0x80A10004);
    r = lifeStep(st, s);
    expect("A10004 non spegne", !r.stopReceive && !st.receiveDead);

    st = {};
    s = {};
    lifeStep(st, s);
    s.haveEvent = true;
    s.eventType = 0x10000000;
    r = lifeStep(st, s);
    expect("evento resume", r.cmd == LifeCmd::ResumeRest);

    st = {};
    s = {};
    lifeStep(st, s);
    s.overlay = 1;
    r = lifeStep(st, s);
    expect("menu PS in pausa", r.cmd == LifeCmd::Pause && r.logBackground && st.exitArmed);
    s.overlay = 0;
    r = lifeStep(st, s);
    expect("ritorno dal tasto PS", r.cmd == LifeCmd::ResumeMenu && r.logResume && !st.wasMenu);

    st = {};
    s = {};
    lifeStep(st, s);
    s.overlay = 1;
    s.sysSus = 1;
    r = lifeStep(st, s);
    expect("menu durante il rest non chiude", r.cmd == LifeCmd::None && r.logSuspend);

    st = {};
    s = {};
    lifeStep(st, s);
    s.overlay = 1;
    r = lifeStep(st, s);
    expect("prepara menu", r.cmd == LifeCmd::Pause && st.wasMenu);
    s.appSus = 1;
    s.sysSus = 0;
    r = lifeStep(st, s);
    expect("appSus dal menu non chiude", r.cmd != LifeCmd::Close && !isCloseSignal(st, s));

    st = {};
    s = {};
    lifeStep(st, s);
    s.appSus = 1;
    s.sysSus = 0;
    r = lifeStep(st, s);
    // appSus positivo = sospensione leggera, non Close.
    expect("appSus senza menu non chiude", r.cmd != LifeCmd::Close && r.logSuspend);

    st = {};
    s = {};
    lifeStep(st, s);
    s.overlay = 1;
    r = lifeStep(st, s);
    expect("menu prima dell'evento", r.cmd == LifeCmd::Pause && st.wasMenu);
    s.haveEvent = true;
    s.eventType = 0x20000000;
    r = lifeStep(st, s);
    expect("evento non-resume non chiude", r.cmd != LifeCmd::Close);

    st = {};
    s = {};
    lifeStep(st, s);
    s.appMsg = true;
    r = lifeStep(st, s);
    expect("AppMessaging non chiude", r.cmd != LifeCmd::Close && !isCloseSignal(st, s));

    st = {};
    s = {};
    lifeStep(st, s);
    s.overlay = 1;
    r = lifeStep(st, s);
    expect("arma per chiusura", r.cmd == LifeCmd::Pause && st.exitArmed);
    s.statusRc = static_cast<int>(0x80B20001);
    r = lifeStep(st, s);
    expect("getstatus ko in menu spegne poll senza Close", r.cmd != LifeCmd::Close && r.stopAll);

    // Smoke 60 s: stesso errore di 01.39/01.40 a ogni tick del monitor (~50 ms).
    {
        LifeState smoke;
        LifeSample tick;
        tick.statusRc = 0;
        const int32_t kErr = -2136932343;  // 0x80A10009
        expect("smoke: errore non è sospeso", !appSuspendedFromRc(kErr));
        const std::time_t t0 = std::time(nullptr);
        unsigned pumps = 0;
        bool closed = false;
        while (std::difftime(std::time(nullptr), t0) < 60.0) {
            tick.appSus = appSuspendedFromRc(kErr) ? 1 : 0;
            tick.appMsg = false;
            tick.haveEvent = false;
            tick.background = 0;
            tick.overlay = 0;
            auto step = lifeStep(smoke, tick);
            ++pumps;
            if (step.cmd == LifeCmd::Close || step.logClose || isCloseSignal(smoke, tick)) {
                closed = true;
                break;
            }
        }
        expect("smoke 60s senza Close", !closed && pumps > 0);
        std::printf("smoke: %u pump in 60s, no exit\n", pumps);
    }

    if (g_fail) {
        std::fprintf(stderr, "%d falliti\n", g_fail);
        return 1;
    }
    std::puts("lifecycle ok");
    return 0;
}
