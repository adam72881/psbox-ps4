// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "media/audio_out.h"

#include "util/log.h"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(XCLOUD_PS5) || defined(XCLOUD_PS4)
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void* ptr);
int sceAudioOutClose(int handle);
int sceAudioOutSetVolume(int handle, int channels, int* vol);
}
#endif

namespace xc::media {

namespace {

constexpr int kRate = 48000;
constexpr unsigned kGrain = 256;
// The queue's bound: past 60 ms (a burst from the network) it is cut back to
// 40 ms, the oldest samples first. It used to hold up to 120 ms: that much
// delay behind the picture (Moonlight keeps 40-50 ms).
constexpr size_t kMaxQueued = kRate * 60 / 1000;
constexpr size_t kTrimTo = kRate * 40 / 1000;

std::mutex g_mutex;
std::deque<float> g_queue;  // interleaved stereo
std::atomic<bool> g_running{false};
std::atomic<float> g_volume{1.0f};
// Measured for the log every ~10 s (under g_mutex): the queue's depth at each
// grain, the grains that found it empty, what was cut.
uint64_t g_depthSum = 0, g_depthMax = 0, g_grains = 0, g_underruns = 0, g_trimmed = 0;
std::thread g_thread;

#if defined(XCLOUD_PS5) || defined(XCLOUD_PS4)
int g_handle = -1;
std::atomic<AudioRoute> g_route{AudioRoute::Main};

void loop() {
    std::vector<float> grain(kGrain * 2);
    while (g_running) {
        int h = g_handle;
        if (h < 0) break;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            size_t depth = g_queue.size() / 2;
            g_depthSum += depth;
            g_depthMax = std::max<uint64_t>(g_depthMax, depth);
            if (depth == 0) ++g_underruns;
            if (++g_grains == 1875) {  // 256-frame grains: ~10 s
                XC_LOGI("audio out: queue %.1f ms on average (max %.1f), %llu empty grains, %.0f ms cut",
                        g_depthSum * 1000.0 / kRate / g_grains, g_depthMax * 1000.0 / kRate,
                        static_cast<unsigned long long>(g_underruns), g_trimmed * 1000.0 / kRate);
                g_depthSum = g_depthMax = g_grains = g_underruns = g_trimmed = 0;
            }
            size_t n = std::min(grain.size(), g_queue.size());
            std::copy(g_queue.begin(), g_queue.begin() + static_cast<long>(n), grain.begin());
            g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<long>(n));
            std::fill(grain.begin() + static_cast<long>(n), grain.end(), 0.0f);
        }
        if (!g_running || g_handle < 0) break;
        if (float v = g_volume.load(); v != 1.0f)
            for (float& s : grain) s = std::clamp(s * v, -1.0f, 1.0f);
        h = g_handle;
        if (h < 0) break;
        // Blocks until the hardware wants the next grain (5.3 ms).
        int rc = sceAudioOutOutput(h, grain.data());
        if (rc < 0) {
            // Handle revocato in chiusura: si esce senza riprovare.
            if (!g_running || g_handle < 0) break;
            static uint64_t failures = 0;
            if (failures++ % 200 == 0)
                XC_LOGW("sceAudioOutOutput: 0x%08x (failure %llu)", static_cast<unsigned>(rc),
                        static_cast<unsigned long long>(failures));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}
#else
std::atomic<AudioRoute> g_route{AudioRoute::Main};
#endif

}  // namespace

bool audioStart() {
    if (g_running) return true;
#if defined(XCLOUD_PS5) || defined(XCLOUD_PS4)
    // Once per process: a second sceAudioOutInit answers "already
    // initialised" (0x8026000E on this firmware, 0x8026000C elsewhere).
    static bool initialised = false;
    if (!initialised) {
        int rc = sceAudioOutInit();
        if (rc < 0 && rc != static_cast<int>(0x8026000C) && rc != static_cast<int>(0x8026000E)) {
            XC_LOGE("sceAudioOutInit: 0x%08x", static_cast<unsigned>(rc));
            return false;
        }
        initialised = true;
    }
    auto openPort = [](int user, int type, int index) {
        int h = sceAudioOutOpen(user, type, index, kGrain, kRate, 4);
        XC_LOGI("audio: open user=%d type=%d idx=%d => 0x%08x", user, type, index,
                static_cast<unsigned>(h));
        if (h > 0) {
            int vol[2] = {32768, 32768};
            int vr = sceAudioOutSetVolume(h, 2, vol);
            XC_LOGI("audio: volume => 0x%08x", static_cast<unsigned>(vr));
        }
        return h;
    };
    // SYSTEM prima: MAIN rifiuta un user id che non ha lanciato il titolo (0x809b0001).
    g_handle = openPort(0xFF, 0, 0);
    for (int idx = 1; g_handle <= 0 && idx <= 3; ++idx) g_handle = openPort(0xFF, 0, idx);
    if (g_handle <= 0) g_handle = openPort(0xFF, 1, 0);
    if (g_handle <= 0) {
        XC_LOGE("sceAudioOutOpen: 0x%08x", static_cast<unsigned>(g_handle));
        return false;
    }
    g_running = true;
    g_thread = std::thread(loop);
    XC_LOGI("audio output open (48 kHz float stereo, handle %d)", g_handle);
#else
    g_running = true;
#endif
    return true;
}

void audioSetVolume(float gain) { g_volume = std::clamp(gain, 0.0f, 2.0f); }

void audioSetRoute(AudioRoute route) {
    g_route.store(route);
}

AudioRoute audioRoute() {
    return g_route.load();
}

void audioStop() {
    if (!g_running) return;
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
#if defined(XCLOUD_PS5) || defined(XCLOUD_PS4)
    if (g_handle >= 0) {
        sceAudioOutClose(g_handle);
        g_handle = -1;
    }
#endif
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
}

void audioAbortForExit() {
    g_running = false;
#if defined(XCLOUD_PS5) || defined(XCLOUD_PS4)
    // Niente sceAudioOutClose: in chiusura di sistema può appendersi.
    // Si invalida l'handle; _exit ripulisce.
    g_handle = -1;
#endif
    // Niente join: il thread vede g_running/g_handle e esce, oppure
    // il processo fa _exit subito dopo.
}

namespace {
bool g_audioHome = false;
}

void audioSuspendForHome() {
#if defined(XCLOUD_PS5) || defined(XCLOUD_PS4)
    if (!g_running.load() || g_audioHome) return;
    int h = g_handle;
    g_running = false;
    g_handle = -1;
    g_audioHome = true;
    // Close sblocca sceAudioOutOutput (un grain, ~5 ms). Non si fa join qui:
    // se il kill arriva durante la chiusura, il segnale fa _exit.
    if (h >= 0) sceAudioOutClose(h);
    XC_LOGI("audio: sospeso (menu PS)");
#else
    (void)g_audioHome;
#endif
}

void audioResumeFromHome() {
#if defined(XCLOUD_PS5) || defined(XCLOUD_PS4)
    if (!g_audioHome) return;
    g_audioHome = false;
    if (g_thread.joinable()) g_thread.join();
    XC_LOGI("audio: riapro dopo il menu PS");
    audioStart();
#endif
}

void audioPush(const float* interleaved, size_t frames) {
    if (!g_running) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.insert(g_queue.end(), interleaved, interleaved + frames * 2);
    if (g_queue.size() > kMaxQueued * 2) {
        size_t cut = g_queue.size() - kTrimTo * 2;
        g_trimmed += cut / 2;
        g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<long>(cut));
    }
}

}  // namespace xc::media
