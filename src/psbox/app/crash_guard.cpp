// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// In chiusura il sistema revoca VideoOut/Audio/memoria mentre i thread le
// usano → SIGSEGV/BUS → CE-34878-0. Qui si logga (write+fsync) e si fa
// subito _exit(0): niente re-raise, niente dtor, niente report di crash.
#include "app/crash_guard.h"

#include "platform/platform.h"
#include "util/log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace xc::app::crash {

namespace {

char g_marker[512] = {};
char g_stage[96] = "starting";
std::atomic<uint64_t> g_beat{0};
std::atomic<uint64_t> g_videoBeat{0};
std::atomic<bool> g_streamWatch{false};
int g_logFd = -1;
uintptr_t g_base = 0;
alignas(16) char g_alt[64 * 1024];

extern "C" void xc_log_stamp(const char* msg);

constexpr size_t kMuslRip = offsetof(ucontext_t, uc_mcontext) + sizeof(greg_t) * REG_RIP;
constexpr size_t kMuslRbp = offsetof(ucontext_t, uc_mcontext) + sizeof(greg_t) * REG_RBP;
constexpr size_t kFbsdRip = 16 + 160;
constexpr size_t kFbsdRbp = 16 + 72;
static_assert(kMuslRip + 8 <= 24 * 8, "musl RIP outside the dumped ucontext");
static_assert(kFbsdRip + 8 <= 24 * 8, "freebsd RIP outside the dumped ucontext");

void append(char* b, int* n, int cap, const char* s) {
    while (s && *s && *n + 1 < cap) b[(*n)++] = *s++;
    if (*n < cap) b[*n] = 0;
}

void appendDec(char* b, int* n, int cap, unsigned v) {
    char tmp[16];
    int i = 0;
    do tmp[i++] = static_cast<char>('0' + (v % 10)), v /= 10; while (v && i < 15);
    while (i && *n + 1 < cap) b[(*n)++] = tmp[--i];
    if (*n < cap) b[*n] = 0;
}

void appendHex(char* b, int* n, int cap, uint64_t v) {
    static const char kHex[] = "0123456789abcdef";
    char tmp[19];
    tmp[0] = '0';
    tmp[1] = 'x';
    for (int i = 0; i < 16; ++i) tmp[2 + i] = kHex[(v >> ((15 - i) * 4)) & 0xf];
    tmp[18] = 0;
    append(b, n, cap, tmp);
}

uint64_t offOf(uint64_t base, uint64_t rip) { return base && rip >= base ? rip - base : rip; }

uint64_t qwordAt(const unsigned char* raw, size_t nraw, size_t off) {
    if (off + 8 > nraw) return 0;
    uint64_t v = 0;
    std::memcpy(&v, raw + off, 8);
    return v;
}

void flushFd(int fd, const char* b, int n) {
    if (fd < 0 || n <= 0) return;
    (void)!::write(fd, b, static_cast<size_t>(n));
    (void)!::fsync(fd);
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTERM: return "SIGTERM";
    case SIGINT: return "SIGINT";
    default: return "?";
    }
}

void writeMarker(const char* kind) {
    if (!g_marker[0]) return;
    int fd = ::open(g_marker, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    auto put = [fd](const char* s) { (void)!::write(fd, s, std::strlen(s)); };
    put(kind);
    put("\n");
    put(g_stage);
    put("\n");
    (void)!::fsync(fd);
    ::close(fd);
}

// Async-signal-safe: solo write/fsync/_exit. Mai malloc, mai re-raise.
void onSignal(int sig, siginfo_t* info, void* uctx) {
    constexpr int kCap = 1400;
    char b[kCap];
    int n = 0;
    append(b, &n, kCap, "exit: crash ");
    append(b, &n, kCap, signalName(sig));
    append(b, &n, kCap, " sig=");
    appendDec(b, &n, kCap, static_cast<unsigned>(sig));
    append(b, &n, kCap, " addr=");
    appendHex(b, &n, kCap, info ? reinterpret_cast<uint64_t>(info->si_addr) : 0);
    append(b, &n, kCap, " thr=");
    appendHex(b, &n, kCap, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self())));

    constexpr size_t kRaw = 24 * 8;
    unsigned char raw[kRaw];
    std::memset(raw, 0, sizeof raw);
    if (uctx) std::memcpy(raw, uctx, sizeof raw);
    uint64_t ripM = qwordAt(raw, kRaw, kMuslRip);
    uint64_t ripF = qwordAt(raw, kRaw, kFbsdRip);
    uint64_t base = static_cast<uint64_t>(g_base);

    append(b, &n, kCap, " rip=");
    appendHex(b, &n, kCap, ripF ? ripF : ripM);
    append(b, &n, kCap, " off=");
    appendHex(b, &n, kCap, offOf(base, ripF ? ripF : ripM));
    append(b, &n, kCap, "\n");

    flushFd(STDERR_FILENO, b, n);
    flushFd(g_logFd, b, n);
    writeMarker(b);

    const char done[] = "exit: _exit after signal\n";
    flushFd(STDERR_FILENO, done, static_cast<int>(sizeof done - 1));
    flushFd(g_logFd, done, static_cast<int>(sizeof done - 1));
    const char fin[] = "exit: processo terminato (segnale)\n";
    flushFd(STDERR_FILENO, fin, static_cast<int>(sizeof fin - 1));
    flushFd(g_logFd, fin, static_cast<int>(sizeof fin - 1));

    // CE-34878-0 nasce dal re-raise / dai dtor. Qui si esce puliti.
    _exit(0);
}

void unblockCloseSignals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
}

}  // namespace

void setLogFd(int fd) { g_logFd = fd; }

void install(const std::string& markerPath) {
    std::strncpy(g_marker, markerPath.c_str(), sizeof g_marker - 1);
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&install), &info) && info.dli_fbase)
        g_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    stack_t ss{};
    ss.ss_sp = g_alt;
    ss.ss_size = sizeof g_alt;
    ss.ss_flags = 0;
    int alt = sigaltstack(&ss, nullptr);
    if (alt != 0)
        XC_LOGW("crash guard: sigaltstack errno %d — lo stack alternativo non c'è", errno);
    XC_LOGI("crash guard: alt stack %d (%zu byte) base %p, fault → _exit(0)", alt, sizeof g_alt,
            reinterpret_cast<void*>(g_base));
    // Niente SA_RESETHAND: se arriva un secondo fault durante il log, si
    // ripete _exit invece del default (report CE).
    struct sigaction fault {};
    fault.__sa_handler.__sa_sigaction = onSignal;
    fault.sa_flags = SA_SIGINFO | (alt == 0 ? SA_ONSTACK : 0);
    sigemptyset(&fault.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &fault, nullptr);
    // 01.46: sigaltstack ha tornato -1 e SIGTERM era SA_ONSTACK. Orbis non
    // consegna il segnale, «Chiudi applicazione» non scrive nulla e il
    // processo resta vivo (keepalive STUN). TERM/INT/QUIT/HUP stanno sullo
    // stack normale, come Itemzflow / RetroArch / HB-Store.
    struct sigaction closeSig {};
    closeSig.__sa_handler.__sa_sigaction = onSignal;
    closeSig.sa_flags = SA_SIGINFO;
    sigemptyset(&closeSig.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGQUIT, SIGHUP}) sigaction(sig, &closeSig, nullptr);
    unblockCloseSignals();
    XC_LOGI("crash guard: SIGTERM/INT/QUIT/HUP sullo stack normale, sbloccati");
    std::set_terminate([] {
        const char* what = "uncaught exception";
        try {
            if (std::current_exception()) std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
            what = "uncaught non-std exception";
        }
        xc_log_stamp(what);
        writeMarker("uncaught exception");
        const char done[] = "exit: _exit after terminate\n";
        flushFd(g_logFd, done, static_cast<int>(sizeof done - 1));
        const char fin[] = "exit: processo terminato (terminate)\n";
        flushFd(g_logFd, fin, static_cast<int>(sizeof fin - 1));
        _exit(0);
    });
}

void noteProgress(const char* line) {
    if (!g_marker[0] || !line || !line[0]) return;
    char buf[96];
    size_t n = 0;
    for (; line[n] && n + 1 < sizeof buf; ++n) {
        char c = line[n];
        if (c == '\n' || c == '\r') break;
        buf[n] = c;
    }
    buf[n] = 0;
    if (!n) return;
    std::memcpy(g_stage, buf, n + 1);
    int fd = ::open(g_marker, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    (void)!::write(fd, "last line\n", 10);
    (void)!::write(fd, buf, n);
    (void)!::write(fd, "\n", 1);
    (void)!::fsync(fd);
    ::close(fd);
}

void setStage(const char* stage) {
    char tmp[sizeof g_stage];
    std::strncpy(tmp, stage, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = 0;
    std::memcpy(g_stage, tmp, sizeof tmp);
}

void heartbeat() { g_beat = platform::nowMs(); }
void videoHeartbeat() { g_videoBeat = platform::nowMs(); }
void setStreamingWatch(bool on) {
    g_streamWatch.store(on);
    if (on) videoHeartbeat();
}

void startWatchdog(uint32_t stallMs) {
    heartbeat();
    videoHeartbeat();
    XC_LOGI("crash guard: stack 8 MiB, stall video/main a 3 s");
    static platform::Thread thread;
    if (!platform::startThread(
            thread,
            [stallMs] {
                bool stalled = false;
                bool videoStalled = false;
                uint64_t last = platform::nowMs();
                for (;;) {
                    platform::sleepMs(1000);
                    uint64_t now = platform::nowMs();
                    if (now - last > 5000) {
                        XC_LOGI("crash guard: the app was suspended for %llu ms (rest mode)",
                                static_cast<unsigned long long>(now - last));
                        heartbeat();
                        videoHeartbeat();
                        last = now;
                        continue;
                    }
                    last = now;
                    uint64_t beat = g_beat.load();
                    uint64_t idle = now > beat ? now - beat : 0;
                    uint64_t vBeat = g_videoBeat.load();
                    uint64_t vIdle = now > vBeat ? now - vBeat : 0;
                    bool streaming = g_streamWatch.load();

                    // Dump precoce (3 s): main o video fermi durante lo stream.
                    if (idle > 3000 || (streaming && vIdle > 3000)) {
                        char line[160];
                        std::snprintf(line, sizeof line,
                                      "watchdog: main idle %llu ms, video idle %llu ms, stream %d, stage %s",
                                      static_cast<unsigned long long>(idle),
                                      static_cast<unsigned long long>(vIdle), streaming ? 1 : 0, g_stage);
                        xc_log_stamp(line);
                        XC_LOGW("%s", line);
                    }

                    if (!stalled && idle > stallMs) {
                        stalled = true;
                        XC_LOGW("crash guard: the main loop has not run for %llu ms (%s)",
                                static_cast<unsigned long long>(idle), g_stage);
                        writeMarker("stalled");
                    } else if (stalled && idle < 1000) {
                        stalled = false;
                        XC_LOGI("crash guard: the main loop is back");
                        ::unlink(g_marker);
                    }
                    if (streaming && !videoStalled && vIdle > stallMs) {
                        videoStalled = true;
                        XC_LOGW("crash guard: video thread idle %llu ms (%s)",
                                static_cast<unsigned long long>(vIdle), g_stage);
                        writeMarker("video-stalled");
                    } else if (videoStalled && (!streaming || vIdle < 1000)) {
                        videoStalled = false;
                        XC_LOGI("crash guard: video thread is back");
                    }
                }
            },
            8u << 20))
        XC_LOGE("crash guard: thread non partito");
}

Previous takePrevious(const std::string& markerPath) {
    Previous p;
    std::string text;
    if (!platform::readFile(markerPath, text)) return p;
    ::unlink(markerPath.c_str());
    size_t nl = text.find('\n');
    p.found = true;
    p.kind = text.substr(0, nl);
    if (nl != std::string::npos) {
        p.stage = text.substr(nl + 1);
        while (!p.stage.empty() && (p.stage.back() == '\n' || p.stage.back() == '\r')) p.stage.pop_back();
    }
    return p;
}

}  // namespace xc::app::crash
