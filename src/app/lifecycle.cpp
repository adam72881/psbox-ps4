// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Tasto PS: pausa disegno, l'app resta aperta. «Chiudi applicazione»:
// spesso non arriva alcun evento e il sistema revoca VideoOut/Audio →
// CE-34878-0 solo DOPO uno stream: thread in Decode/AudioOut/resetCallbacks
// mentre il sistema revoca le risorse. Regola 01.34:
// 1) GetStatus+ReceiveEvent ogni frame sul main (e backup sul monitor);
// 2) stop immediato senza Close/join/resetCallbacks/DeleteDecoder;
// 3) flag g_appStopping prima di ogni syscall video/audio;
// 4) _exit(0) con log fsync di ogni passo; fault → crash_guard → _exit(0).
#include "app/ps5_app.h"

#include "app/lifecycle_decide.h"

#include "app/stream_player.h"
#include "display/display.h"
#include "input/controller.h"
#include "media/audio_out.h"
#include "platform/platform.h"
#include "stream/stream_session.h"
#include "util/log.h"

#include <atomic>
#include <cstdio>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <unistd.h>

namespace xc::app {

std::atomic<bool> g_appStopping{false};
std::atomic<bool> g_psMenu{false};

namespace {

std::atomic<int> g_lifeCmd{0};  // 0 niente, 1 ripresa dal rest
std::atomic<bool> g_dying{false};
platform::Thread g_monitor;
std::mutex g_lifeMu;
LifeState g_life;

constexpr size_t kStatusBytes = 64 * 1024;
constexpr size_t kEventBytes = 64 * 1024;
constexpr size_t kAppMsgBytes = 4 * 1024;
constexpr uint64_t kFence = 0x5853435354415400ull;  // "XCSTAT"

struct StatusStore {
    alignas(64) uint8_t raw[kStatusBytes];
    uint64_t fence;
};
struct EventStore {
    alignas(64) uint8_t raw[kEventBytes];
    uint64_t fence;
};
StatusStore g_statusStore{};
EventStore g_eventStore{};
alignas(64) uint8_t g_appMsgBuf[kAppMsgBytes];

extern "C" uint32_t sceSysmoduleLoadModuleInternal(uint32_t id);
extern "C" int32_t sceSystemServiceGetStatus(void* status);
extern "C" int32_t sceSystemServiceReceiveEvent(void* event);
extern "C" int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void*, int*);
extern "C" int sceKernelDlsym(int handle, const char* name, void** addr);
extern "C" int sceKernelGetModuleList(int* list, size_t size, size_t* available);
extern "C" void xc_set_exit_hook(void (*fn)(void));
extern "C" void xc_log_stamp(const char* msg);

constexpr uint32_t kSystemServiceModule = 0x80000010;

using TryReceiveMsgFn = int32_t (*)(void* msg, size_t size);
using ReceiveMsgFn = int32_t (*)(void* msg, size_t size, int32_t* unk);
// SDK / shadPS4: sceSystemServiceIsAppSuspended(void) — non-zero = sospesa.
using IsAppSuspendedFn = int32_t (*)();
using IsShellUiFgFn = int32_t (*)();
using GetPsButtonFn = int32_t (*)(int32_t* out);
TryReceiveMsgFn g_tryReceiveMsg = nullptr;
ReceiveMsgFn g_receiveMsg = nullptr;
IsAppSuspendedFn g_isAppSuspended = nullptr;
IsShellUiFgFn g_isShellUiFg = nullptr;
GetPsButtonFn g_getPsButton = nullptr;
int g_sysHandle = -1;
bool g_modsTried = false;
uint64_t g_lastPumpMs = 0;
int g_pumpCount = 0;

void postCmd(int cmd) { g_lifeCmd.store(cmd); }

// Teardown senza API che possono appendersi (VideoOutClose, AudioOutClose,
// PeerConnection::close, resetCallbacks, join, DeleteDecoder). Ogni passo
// è loggato con fsync: se CE-34878-0 torna, l'ultima riga dice dove.
// Poi _exit(0) PRIMA che il sistema revochi decoder/video/audio — niente dtor.
void dieNow(const char* why) {
    if (g_dying.exchange(true)) {
        xc_log_stamp("[EXIT] dieNow re-enter → _exit");
        _exit(0);
    }
    // Flag subito: i thread video/audio non entrano più in Decode/Output.
    g_appStopping.store(true);
    g_psMenu.store(false);
    xc_log_stamp("[EXIT] chiusura richiesta dal sistema");
    XC_LOGI("[EXIT] chiusura richiesta dal sistema (%s)", why ? why : "");
    char line[96];
    std::snprintf(line, sizeof line, "[EXIT] dieNow (%s)", why ? why : "");
    xc_log_stamp(line);

    xc_log_stamp("exit: stream requestStop");
    stream::requestStop();

    xc_log_stamp("exit: audioAbort (no AudioOutClose)");
    media::audioAbortForExit();

    xc_log_stamp("exit: videoRelease (no VideoOutClose)");
    display::releaseForExit();

    xc_log_stamp("exit: player abort (no resetCallbacks/join)");
    if (g_playerMutex.try_lock()) {
        if (g_player) g_player->abortForExit();
        g_playerMutex.unlock();
        xc_log_stamp("exit: player abort ok");
    } else {
        // Mutex busy: i flag sopra bastano; il video loop vede g_appStopping.
        xc_log_stamp("exit: player mutex busy (flags already set)");
    }

    // ATTRIBUTE=0: niente LoadExec. _exit evita i dtor di libdatachannel /
    // Videodec2 che in chiusura di sistema sono CE-34878-0.
    xc_log_stamp("exit: _exit(0)");
    xc_log_stamp("exit: processo terminato (dieNow)");
    _exit(0);
}

int dlsymAny(const char* name, void** out) {
    *out = nullptr;
    if (g_sysHandle > 0 && sceKernelDlsym(g_sysHandle, name, out) == 0 && *out) return 0;
    int list[96] = {};
    size_t avail = 0;
    if (sceKernelGetModuleList(list, 96, &avail) == 0) {
        size_t n = avail > 96 ? 96 : avail;
        for (size_t i = 0; i < n; ++i) {
            if (list[i] <= 0) continue;
            if (sceKernelDlsym(list[i], name, out) == 0 && *out) {
                g_sysHandle = list[i];
                return 0;
            }
        }
    }
    return -1;
}

void ensureModules() {
    if (g_modsTried) return;
    g_modsTried = true;
    xc_log_stamp("exit: moduli enter");
    uint32_t mod = sceSysmoduleLoadModuleInternal(kSystemServiceModule);
    char line[72];
    std::snprintf(line, sizeof line, "exit: SystemService 0x%08x", static_cast<unsigned>(mod));
    xc_log_stamp(line);

    const char* sysPaths[] = {
        "/system/common/lib/libSceSystemService.sprx",
        "/system/priv/lib/libSceSystemService.sprx",
    };
    for (const char* p : sysPaths) {
        int rc = sceKernelLoadStartModule(p, 0, nullptr, 0, nullptr, nullptr);
        if (rc > 0) {
            g_sysHandle = rc;
            break;
        }
    }

    // AppMessaging è esportato da libSceSystemService (non uno sprx separato).
    void* sym = nullptr;
    if (dlsymAny("sceAppMessagingTryReceiveMsg", &sym) == 0) {
        g_tryReceiveMsg = reinterpret_cast<TryReceiveMsgFn>(sym);
        xc_log_stamp("exit: AppMessaging Try ok (SystemService)");
    } else {
        const char* msgPaths[] = {
            "/system/common/lib/libSceSystemService.sprx",
            "/system/priv/lib/libSceSystemService.sprx",
            "/system/common/lib/libSceAppMessaging.sprx",
            "/system/priv/lib/libSceAppMessaging.sprx",
        };
        for (const char* p : msgPaths) {
            int rc = sceKernelLoadStartModule(p, 0, nullptr, 0, nullptr, nullptr);
            if (rc > 0 && sceKernelDlsym(rc, "sceAppMessagingTryReceiveMsg", &sym) == 0 && sym) {
                g_tryReceiveMsg = reinterpret_cast<TryReceiveMsgFn>(sym);
                g_sysHandle = rc;
                xc_log_stamp("exit: AppMessaging Try ok (sprx)");
                break;
            }
        }
        if (!g_tryReceiveMsg) xc_log_stamp("exit: AppMessaging Try assente");
    }
    sym = nullptr;
    if (dlsymAny("sceAppMessagingReceiveMsg", &sym) == 0) {
        g_receiveMsg = reinterpret_cast<ReceiveMsgFn>(sym);
        xc_log_stamp("exit: AppMessaging Receive ok");
    }

    sym = nullptr;
    if (dlsymAny("sceSystemServiceIsAppSuspended", &sym) == 0) {
        g_isAppSuspended = reinterpret_cast<IsAppSuspendedFn>(sym);
        xc_log_stamp("exit: IsAppSuspended ok");
    }
    sym = nullptr;
    if (dlsymAny("sceSystemServiceIsShellUiFgAndGameBgCpuMode", &sym) == 0) {
        g_isShellUiFg = reinterpret_cast<IsShellUiFgFn>(sym);
        xc_log_stamp("exit: IsShellUiFgAndGameBgCpuMode ok");
    }
    sym = nullptr;
    if (dlsymAny("sceSystemServiceGetPSButtonEvent", &sym) == 0) {
        g_getPsButton = reinterpret_cast<GetPsButtonFn>(sym);
        xc_log_stamp("exit: GetPSButtonEvent ok");
    } else {
        xc_log_stamp("exit: GetPSButtonEvent assente");
    }
    xc_log_stamp("exit: moduli leave");
}

bool pollAppMessaging() {
    // Solo diagnostica: un messaggio non significa «Chiudi» (su 11.52
    // AppMessaging è spesso assente; se presente non chiude l'app).
    std::memset(g_appMsgBuf, 0, sizeof g_appMsgBuf);
    if (g_tryReceiveMsg) {
        int32_t rc = g_tryReceiveMsg(g_appMsgBuf, sizeof g_appMsgBuf);
        if (rc == 0) {
            int32_t kind = 0;
            std::memcpy(&kind, g_appMsgBuf, sizeof kind);
            char line[80];
            std::snprintf(line, sizeof line, "exit: AppMessaging Try 0x%08x (no close)",
                          static_cast<unsigned>(kind));
            xc_log_stamp(line);
        }
    }
    if (g_receiveMsg) {
        std::memset(g_appMsgBuf, 0, sizeof g_appMsgBuf);
        int32_t unk = 0;
        int32_t rc = g_receiveMsg(g_appMsgBuf, sizeof g_appMsgBuf, &unk);
        if (rc == 0) {
            int32_t kind = 0;
            std::memcpy(&kind, g_appMsgBuf, sizeof kind);
            char line[80];
            std::snprintf(line, sizeof line, "exit: AppMessaging Recv 0x%08x (no close)",
                          static_cast<unsigned>(kind));
            xc_log_stamp(line);
        }
    }
    return false;  // mai campione appMsg → mai Close
}

void onResume(const char* why) {
    XC_LOGI("exit: ripresa (%s)", why);
    g_psMenu.store(false);
    media::audioStop();
    display::resumeAfterRest();
    g_cancel.store(true);
}

void onTerm(int sig) {
    (void)sig;
    const char a[] = "exit: segnale TERM/USR\n";
    const char b[] = "exit: processo terminato (USR)\n";
    (void)!write(STDERR_FILENO, a, sizeof a - 1);
    (void)!write(STDERR_FILENO, b, sizeof b - 1);
    xc_log_stamp("exit: segnale");
    xc_log_stamp("exit: processo terminato (USR)");
    _exit(0);
}

extern "C" void xc_on_libc_exit(void) {
    xc_log_stamp("exit: exit()");
    xc_log_stamp("exit: processo terminato (exit)");
    _exit(0);
}

void softReleaseStreamResources(const char* why) {
    // HB-Store/Itemzflow: in background si rilasciano VO/Audio subito così
    // «Chiudi applicazione» non resta bloccato su Decode/Output.
    xc_log_stamp("exit: soft-release stream");
    XC_LOGI("exit: soft-release stream (%s)", why ? why : "");
    g_psMenu.store(true);
    // Niente requestStop: è globale e il tentativo dopo usciva con «chiusura».
    media::audioAbortForExit();
    display::releaseForExit();
    if (g_playerMutex.try_lock()) {
        if (g_player) g_player->abortForExit();
        g_playerMutex.unlock();
    }
    g_cancel.store(true);
}

void applyLifeResult(const LifeResult& step) {
    if (step.logSuspend) {
        xc_log_stamp("exit: sospensione");
        XC_LOGI("exit: sospensione");
    }
    if (step.logBackground) {
        // Tasto PS / overlay: l'app resta aperta.
        xc_log_stamp("exit: menu PS, l'app resta aperta");
        XC_LOGI("exit: menu PS, l'app resta aperta");
    }
    if (step.logResume) XC_LOGI("exit: ripresa chiesta");
    if (step.logClose) {
        xc_log_stamp("exit: chiusura chiesta");
        XC_LOGI("exit: chiusura chiesta");
    }
    if (step.stopReceive) XC_LOGE("exit: receive disattivato");
    if (step.homeSuspend) {
        // Main thread: present() ha già visto g_psMenu e non è in SubmitFlip.
        display::suspendForHome();
        media::audioSuspendForHome();
        input::suspendForHome();
        xc_log_stamp("exit: bg=1 risorse rilasciate");
        XC_LOGI("exit: bg=1 VideoOut/audio/pad rilasciati, flip fermi, app sospesa");
        xc_log_stamp("exit: Chiudi dal menu = SIGTERM sullo stack normale");
    }
    if (step.homeResume) {
        input::resumeFromHome();
        media::audioResumeFromHome();
        display::resumeFromHome();
        g_psMenu.store(false);
        if (g_playerMutex.try_lock()) {
            if (g_player) g_player->requestKeyframe();
            g_playerMutex.unlock();
        }
        xc_log_stamp("exit: ripresa dal menu PS");
        XC_LOGI("exit: ripresa dal menu PS (video, audio, pad)");
    }
    if (step.releaseStream) softReleaseStreamResources("bg>300ms");
    if (step.resumeStream) {
        XC_LOGI("exit: ripresa dopo soft-release (VideoOut)");
        media::audioStop();
        display::resumeAfterRest();
        g_psMenu.store(false);
        g_cancel.store(true);  // torna in libreria; nuova partita = nuovo stream
    }
    if (step.cmd == LifeCmd::Close) {
        g_appStopping.store(true);
        dieNow("sistema");
    } else if (step.cmd == LifeCmd::Pause) {
        // Flag PRIMA di qualsiasi pad: il main non deve fare sendInput pieno
        // e il monitor NON deve toccare il DataChannel (in 01.33 restava
        // appeso su send → niente più getstatus mon → Chiudi invisibile).
        g_psMenu.store(true);
        xc_log_stamp("exit: pausa menu PS (teardown armato)");
        XC_LOGI("exit: pausa menu PS (teardown armato)");
        // Solo memoria (armNeutralPad via sendInput in menu). Niente send DC.
        // Il main chiama pumpFast e manda il neutro sul canale input.
        if (g_playerMutex.try_lock()) {
            if (g_player) {
                input::ControllerState neutral;
                for (int i = 0; i < input::kMaxPads; ++i) g_player->sendInput(neutral, i);
            }
            g_playerMutex.unlock();
        } else {
            xc_log_stamp("exit: pad neutro saltato (mutex busy)");
        }
    } else if (step.cmd == LifeCmd::ResumeMenu) {
        if (!step.resumeStream) {
            g_psMenu.store(false);
            XC_LOGI("exit: ripresa dal menu PS, disegno ripreso");
            // Decode era in pausa: serve un keyframe fresco.
            if (g_playerMutex.try_lock()) {
                if (g_player) g_player->requestKeyframe();
                g_playerMutex.unlock();
            }
        }
    } else if (step.cmd == LifeCmd::ResumeRest) {
        g_psMenu.store(false);
        postCmd(1);
    }
}

// Una passata GetStatus + drain ReceiveEvent + AppMessaging.
// Chiamata dal main ogni frame e dal monitor come backup.
void pumpLifecycle(bool fromMain) {
    if (g_appStopping.load() || g_dying.load()) return;
    ensureModules();

    std::unique_lock<std::mutex> lock(g_lifeMu, std::try_to_lock);
    if (!lock.owns_lock()) return;  // l'altro thread sta già pompando

    LifeState& st = g_life;
    if (st.statusDead) return;

    uint64_t now = platform::nowMs();
    bool woke = g_lastPumpMs && now > g_lastPumpMs + 5000;
    g_lastPumpMs = now;
    bool inMenu = g_psMenu.load() || st.wasMenu;
    ++g_pumpCount;
    // Niente stamp periodico in menu: l'fsync a raffica teneva il processo
    // occupato e il sistema non riusciva a chiuderlo (app «danneggiata»).
    bool trace = g_pumpCount <= 3;

    LifeSample sample;
    sample.woke = woke;

    std::memset(g_statusStore.raw, 0, kStatusBytes);
    g_statusStore.fence = kFence;
    if (trace) xc_log_stamp(fromMain ? "exit: getstatus main" : "exit: getstatus mon");
    int rc = sceSystemServiceGetStatus(g_statusStore.raw);
    if (g_statusStore.fence != kFence) {
        XC_LOGE("exit: getstatus overflow, monitor spento");
        st.statusDead = true;
        return;
    }
    sample.statusRc = rc;
    if (rc == 0) {
        SysStatusFields fields{};
        std::memcpy(&fields, g_statusStore.raw, sizeof fields);
        sample.eventNum = fields.eventNum;
        sample.overlay = fields.isSystemUiOverlaid;
        sample.background = fields.isInBackgroundExecution;
        sample.cpuMode7 = fields.isCpuMode7CpuNormal;
        sample.streaming = fields.isGameLiveStreamingOnAir;
        sample.cpu = fields.isCpuMode7CpuNormal;
        sample.sysSus = 0;
        sample.appSus = 0;
        static int shown = 0;
        static int32_t bg = 0, overlay = 0, cpu7 = 0, stream = 0, evn = 0;
        bool changed = !shown || bg != fields.isInBackgroundExecution || overlay != fields.isSystemUiOverlaid ||
                       cpu7 != fields.isCpuMode7CpuNormal || stream != fields.isGameLiveStreamingOnAir ||
                       evn != fields.eventNum;
        if (changed) {
            char line[160];
            std::snprintf(line, sizeof line, "exit: status bg %d ov %d cpu7 %d live %d ev %d main %d",
                          fields.isInBackgroundExecution, fields.isSystemUiOverlaid,
                          fields.isCpuMode7CpuNormal, fields.isGameLiveStreamingOnAir,
                          fields.eventNum, fromMain ? 1 : 0);
            xc_log_stamp(line);
            XC_LOGI("%s", line);
            bg = fields.isInBackgroundExecution;
            overlay = fields.isSystemUiOverlaid;
            cpu7 = fields.isCpuMode7CpuNormal;
            stream = fields.isGameLiveStreamingOnAir;
            evn = fields.eventNum;
            shown = 1;
        }
    } else {
        char line[48];
        std::snprintf(line, sizeof line, "exit: getstatus 0x%08x", static_cast<unsigned>(rc));
        xc_log_stamp(line);
        XC_LOGE("%s", line);
    }

    // ReceiveEvent ogni giro del main e del monitor in menu (Chiudi spesso
    // non mette eventNum>0). 0x80a10004 = coda vuota (normale).
    bool tryRecv = !st.receiveDead && (fromMain || sample.eventNum > 0 || inMenu || st.exitArmed ||
                                       (g_pumpCount % 5 == 0));
    if (tryRecv) {
        for (int i = 0; i < 16 && !st.receiveDead; ++i) {
            std::memset(g_eventStore.raw, 0, kEventBytes);
            g_eventStore.fence = kFence;
            int er = sceSystemServiceReceiveEvent(g_eventStore.raw);
            if (g_eventStore.fence != kFence) {
                XC_LOGE("exit: receive overflow");
                st.receiveDead = true;
                sample.receiveRc = -1;
                break;
            }
            sample.receiveRc = er;
            if (er == 0) {
                int32_t evType = 0;
                std::memcpy(&evType, g_eventStore.raw, sizeof evType);
                sample.haveEvent = true;
                sample.eventType = evType;
                char line[64];
                std::snprintf(line, sizeof line, "exit: evento 0x%08x", static_cast<unsigned>(evType));
                xc_log_stamp(line);
                XC_LOGI("%s", line);
                // Nessun evento SystemService chiude l'app (ricerca A1–A4 /
                // log 01.39). Resume → ripresa; resto solo diagnostica.
                (void)terminateEvent(evType);
                continue;
            }
            if (receiveEmpty(er)) {
                if (sample.eventNum > 0 && (fromMain || (inMenu && g_pumpCount % 40 == 0))) {
                    char line[48];
                    std::snprintf(line, sizeof line, "exit: receive vuoto 0x%08x",
                                  static_cast<unsigned>(er));
                    xc_log_stamp(line);
                }
                break;
            }
            char line[48];
            std::snprintf(line, sizeof line, "exit: receive 0x%08x", static_cast<unsigned>(er));
            xc_log_stamp(line);
            XC_LOGI("%s", line);
            break;
        }
    }

    if (g_isAppSuspended) {
        int32_t rc = g_isAppSuspended();
        // Solo rc > 0 = sospesa. Negativo = SCE error (01.39/01.40: 0x80A10009
        // al boot → falso Close). Si logga l'errore una sola volta.
        if (appSuspendedFromRc(rc)) {
            sample.appSus = 1;
            char line[56];
            std::snprintf(line, sizeof line, "exit: IsAppSuspended rc %d", static_cast<int>(rc));
            xc_log_stamp(line);
        } else if (rc < 0) {
            static bool loggedErr = false;
            if (!loggedErr) {
                loggedErr = true;
                char line[72];
                std::snprintf(line, sizeof line, "exit: IsAppSuspended err 0x%08x (ignorato)",
                              static_cast<unsigned>(rc));
                xc_log_stamp(line);
                XC_LOGW("%s", line);
            }
        }
    }

    // Solo diagnostica, una volta per valore: ShellUI in FG è il menu PS, non Chiudi.
    if (g_isShellUiFg && (inMenu || st.exitArmed)) {
        int32_t shellFg = g_isShellUiFg();
        static int32_t lastShell = 0x7fffffff;
        if (shellFg != lastShell) {
            lastShell = shellFg;
            char line[64];
            std::snprintf(line, sizeof line, "exit: ShellUiFg rc %d", static_cast<int>(shellFg));
            xc_log_stamp(line);
        }
    }

    // Solo diagnostica: il tasto PS non chiude (Pause via overlay).
    if (g_getPsButton && inMenu) {
        int32_t ps = 0;
        static int32_t lastPs = 0;
        if (g_getPsButton(&ps) == 0) {
            if (ps && !lastPs) {
                char line[48];
                std::snprintf(line, sizeof line, "exit: PSButton %d (no close)", static_cast<int>(ps));
                xc_log_stamp(line);
            }
            lastPs = ps;
        }
    }

    sample.appMsg = pollAppMessaging();

    LifeResult step = lifeStep(st, sample);
    // Un buco di 5 s col menu PS aperto non è un risveglio dal rest: ResumeRest
    // azzererebbe g_psMenu e il main tornerebbe a fare flip mentre ShellUI è in FG.
    if ((sample.background != 0 || sample.overlay != 0) && step.cmd == LifeCmd::ResumeRest) {
        step.cmd = LifeCmd::None;
        step.logResume = false;
    }
    // bg=1: stop flip subito (anche dal monitor). Il main chiude VideoOut,
    // audio e pad una volta sola. Lo stream resta in pausa e riprende al
    // ritorno: niente requestStop, il tasto PS non chiude l'app.
    if (sample.background != 0 || sample.overlay != 0) {
        g_psMenu.store(true);
        display::holdFlips(true);
        if (!st.bgSinceMs) st.bgSinceMs = now;
    }
    if (sample.background != 0) {
        if (fromMain && !st.homeSuspended) {
            st.homeSuspended = true;
            step.homeSuspend = true;
        }
    } else if (fromMain && st.homeSuspended && sample.overlay == 0) {
        st.homeSuspended = false;
        st.bgSinceMs = 0;
        step.homeResume = true;
    } else if (!sample.overlay) {
        st.bgSinceMs = 0;
        if (fromMain && !st.homeSuspended) display::holdFlips(false);
    }
    if (step.stopAll) {
        xc_log_stamp("exit: status dead");
        return;
    }
    lock.unlock();
    applyLifeResult(step);
}

void monitorLoop() {
    ensureModules();
    sigset_t open;
    sigemptyset(&open);
    sigaddset(&open, SIGTERM);
    sigaddset(&open, SIGINT);
    sigaddset(&open, SIGQUIT);
    sigaddset(&open, SIGHUP);
    pthread_sigmask(SIG_UNBLOCK, &open, nullptr);
    xc_log_stamp("exit: monitor avviato");
    while (!g_appStopping.load() && !g_dying.load()) {
        pumpLifecycle(false);
        // In menu/overlay: poll più stretto — Chiudi spesso non manda eventi
        // e bisogna _exit prima che il sistema revochi Videodec2/Audio/VideoOut.
        // In menu non si gira a 10 ms: il poll stretto più gli fsync tenevano
        // il processo sveglio e «Chiudi applicazione» non passava.
        platform::sleepMs(g_psMenu.load() || g_life.wasMenu || g_life.exitArmed ? 100 : 50);
    }
    xc_log_stamp("exit: monitor fermo");
    XC_LOGI("exit: monitor fermo");
}

}  // namespace

void installExitHooks() {
    xc_set_exit_hook(xc_on_libc_exit);
    // Fault → crash_guard (installato prima in main). Qui USR + conferma
    // che TERM/INT restano su _exit se qualcuno li ha sovrascritti.
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa {};
    sa.sa_handler = onTerm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    for (int sig : {SIGUSR1, SIGUSR2}) sigaction(sig, &sa, nullptr);
    XC_LOGI("exit: agganci pronti (SIGTERM stack normale → _exit, tasto PS non chiude)");
    XC_LOGI("exit: monitor stack 8 MiB, status %zu, evento %zu; pump anche sul main ogni frame",
            kStatusBytes, kEventBytes);
    ensureModules();
    if (!platform::startThread(g_monitor, monitorLoop, 8u << 20)) XC_LOGE("exit: monitor non partito");
}

void pollLifecycle() {
    // Ogni frame del main: GetStatus + ReceiveEvent. Non solo il comando
    // rest-mode — altrimenti «Chiudi applicazione» arriva tardi o mai.
    pumpLifecycle(true);
    int cmd = g_lifeCmd.exchange(0);
    if (cmd == 1) onResume("rest");
}

}  // namespace xc::app
