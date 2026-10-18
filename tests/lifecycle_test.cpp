// g++ -std=c++20 -I src -o /tmp/lifecycle_test tests/lifecycle_test.cpp
#include "app/lifecycle_decide.h"

#include <cstdio>

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
    using xc::app::lifeStep;
    using xc::app::receiveEmpty;
    using xc::app::terminateEvent;

    expect("status 12", sizeof(xc::app::SysStatusFields) == 12);
    expect("evento 8196", sizeof(xc::app::SysEventFields) == 8196);
    expect("coda vuota B20004", receiveEmpty(static_cast<int>(0x80B20004)));
    expect("coda vuota A10004", receiveEmpty(static_cast<int>(0x80A10004)));
    expect("zero non e vuoto", !receiveEmpty(0));
    expect("receive ko non vuoto", !receiveEmpty(static_cast<int>(0x80B20001)));
    expect("terminate 0x10000010", terminateEvent(0x10000010));
    expect("resume non terminate", !terminateEvent(0x10000000));

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

    // woke non maschera la chiusura.
    st = {};
    s = {};
    lifeStep(st, s);
    s.woke = true;
    s.haveEvent = true;
    s.eventType = 0x10000010;
    r = lifeStep(st, s);
    expect("woke+terminate chiude", r.cmd == LifeCmd::Close && r.logClose);

    st = {};
    s = {};
    s.statusRc = static_cast<int>(0x80B20001);
    r = lifeStep(st, s);
    expect("getstatus ko spegne", r.stopAll && st.statusDead && r.cmd == LifeCmd::None);
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
    expect("appSus dal menu chiude", r.cmd == LifeCmd::Close && r.logClose);

    st = {};
    s = {};
    lifeStep(st, s);
    s.appSus = 1;
    s.sysSus = 0;
    r = lifeStep(st, s);
    expect("appSus senza menu chiude", r.cmd == LifeCmd::Close && r.logClose);

    st = {};
    s = {};
    lifeStep(st, s);
    s.overlay = 1;
    r = lifeStep(st, s);
    expect("menu prima dell'evento", r.cmd == LifeCmd::Pause && st.wasMenu);
    s.haveEvent = true;
    s.eventType = 0x20000000;
    r = lifeStep(st, s);
    expect("evento non-resume chiude", r.cmd == LifeCmd::Close && r.logClose);

    st = {};
    s = {};
    lifeStep(st, s);
    s.appMsg = true;
    r = lifeStep(st, s);
    expect("AppMessaging chiude", r.cmd == LifeCmd::Close && r.logClose);

    // Chiudi senza evento: GetStatus fallisce con menu/armato.
    st = {};
    s = {};
    lifeStep(st, s);
    s.overlay = 1;
    r = lifeStep(st, s);
    expect("arma per chiusura", r.cmd == LifeCmd::Pause && st.exitArmed);
    s.statusRc = static_cast<int>(0x80B20001);
    r = lifeStep(st, s);
    expect("getstatus ko in menu chiude", r.cmd == LifeCmd::Close && r.logClose && r.stopAll);

    if (g_fail) {
        std::fprintf(stderr, "%d falliti\n", g_fail);
        return 1;
    }
    std::puts("lifecycle ok");
    return 0;
}
