/* SPDX-License-Identifier: GPL-3.0-or-later
 * Contatori permanenti del percorso di ricezione (xcloud.log, 1 Hz).
 * 01.56: thread dedicato — non dipende dal worker bloccato in start().
 */
#include "stream/recv_diag.h"

#include "platform/platform.h"
#include "util/log.h"

#include <atomic>
#include <cstring>

namespace {

std::atomic<uint64_t> g_poll_recv{0};
std::atomic<uint64_t> g_dtls_incoming{0};
std::atomic<uint64_t> g_dtls_push_ok{0};
std::atomic<uint64_t> g_dtls_push_drop{0};
std::atomic<uint64_t> g_dorecv{0};
std::atomic<uint64_t> g_consent_sent{0};
std::atomic<uint64_t> g_consent_recv{0};
std::atomic<uint64_t> g_rtp_a{0};
std::atomic<uint64_t> g_rtp_b{0};
std::atomic<uint32_t> g_ssrc_a{0};
std::atomic<uint32_t> g_ssrc_b{0};
std::atomic<int> g_inside_push{0};
std::atomic<int> g_queue_size{0};

std::atomic<int> g_diagRun{0};
platform::Thread g_diagThread;

void logRecvOnce() {
    PsboxRecvSnap snap{};
    psbox_rd_snapshot(&snap);
    XC_LOGI(
        "recv: poll=%llu dtls_in=%llu push_ok=%llu push_drop=%llu dorecv=%llu q=%d inside=%d "
        "consent_tx=%llu consent_rx=%llu rtp[%08x]=%llu rtp[%08x]=%llu",
        static_cast<unsigned long long>(snap.poll_recv),
        static_cast<unsigned long long>(snap.dtls_incoming),
        static_cast<unsigned long long>(snap.dtls_push_ok),
        static_cast<unsigned long long>(snap.dtls_push_drop),
        static_cast<unsigned long long>(snap.dorecv), snap.queue_size, snap.inside_push,
        static_cast<unsigned long long>(snap.consent_sent),
        static_cast<unsigned long long>(snap.consent_recv), snap.rtp_ssrc_a,
        static_cast<unsigned long long>(snap.rtp_a), snap.rtp_ssrc_b,
        static_cast<unsigned long long>(snap.rtp_b));
}

}  // namespace

extern "C" void psbox_rd_poll_recv(void) { g_poll_recv.fetch_add(1, std::memory_order_relaxed); }

extern "C" void psbox_rd_dtls_incoming(size_t) { g_dtls_incoming.fetch_add(1, std::memory_order_relaxed); }

extern "C" void psbox_rd_dtls_push(int ok, size_t qsize) {
    if (ok)
        g_dtls_push_ok.fetch_add(1, std::memory_order_relaxed);
    else
        g_dtls_push_drop.fetch_add(1, std::memory_order_relaxed);
    g_queue_size.store(static_cast<int>(qsize), std::memory_order_relaxed);
}

extern "C" void psbox_rd_dorecv(void) { g_dorecv.fetch_add(1, std::memory_order_relaxed); }

extern "C" void psbox_rd_push_enter(void) { g_inside_push.store(1, std::memory_order_relaxed); }

extern "C" void psbox_rd_push_leave(void) { g_inside_push.store(0, std::memory_order_relaxed); }

extern "C" void psbox_rd_consent_sent(void) { g_consent_sent.fetch_add(1, std::memory_order_relaxed); }

extern "C" void psbox_rd_consent_recv(void) { g_consent_recv.fetch_add(1, std::memory_order_relaxed); }

extern "C" void psbox_rd_rtp(uint32_t ssrc) {
    uint32_t a = g_ssrc_a.load(std::memory_order_relaxed);
    if (!a) {
        if (g_ssrc_a.compare_exchange_strong(a, ssrc, std::memory_order_relaxed)) {
            g_rtp_a.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (ssrc == g_ssrc_a.load(std::memory_order_relaxed)) {
        g_rtp_a.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t b = g_ssrc_b.load(std::memory_order_relaxed);
    if (!b) g_ssrc_b.compare_exchange_strong(b, ssrc, std::memory_order_relaxed);
    if (ssrc == g_ssrc_b.load(std::memory_order_relaxed))
        g_rtp_b.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void psbox_rd_snapshot(PsboxRecvSnap* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->poll_recv = g_poll_recv.load(std::memory_order_relaxed);
    out->dtls_incoming = g_dtls_incoming.load(std::memory_order_relaxed);
    out->dtls_push_ok = g_dtls_push_ok.load(std::memory_order_relaxed);
    out->dtls_push_drop = g_dtls_push_drop.load(std::memory_order_relaxed);
    out->dorecv = g_dorecv.load(std::memory_order_relaxed);
    out->consent_sent = g_consent_sent.load(std::memory_order_relaxed);
    out->consent_recv = g_consent_recv.load(std::memory_order_relaxed);
    out->rtp_a = g_rtp_a.load(std::memory_order_relaxed);
    out->rtp_b = g_rtp_b.load(std::memory_order_relaxed);
    out->rtp_ssrc_a = g_ssrc_a.load(std::memory_order_relaxed);
    out->rtp_ssrc_b = g_ssrc_b.load(std::memory_order_relaxed);
    out->inside_push = g_inside_push.load(std::memory_order_relaxed);
    out->queue_size = g_queue_size.load(std::memory_order_relaxed);
}

extern "C" void psbox_rd_reset(void) {
    g_poll_recv.store(0, std::memory_order_relaxed);
    g_dtls_incoming.store(0, std::memory_order_relaxed);
    g_dtls_push_ok.store(0, std::memory_order_relaxed);
    g_dtls_push_drop.store(0, std::memory_order_relaxed);
    g_dorecv.store(0, std::memory_order_relaxed);
    g_consent_sent.store(0, std::memory_order_relaxed);
    g_consent_recv.store(0, std::memory_order_relaxed);
    g_rtp_a.store(0, std::memory_order_relaxed);
    g_rtp_b.store(0, std::memory_order_relaxed);
    g_ssrc_a.store(0, std::memory_order_relaxed);
    g_ssrc_b.store(0, std::memory_order_relaxed);
    g_inside_push.store(0, std::memory_order_relaxed);
    g_queue_size.store(0, std::memory_order_relaxed);
}

extern "C" void psbox_rd_start_logger(void) {
    if (g_diagRun.exchange(1)) return;
    if (!platform::startThread(
            g_diagThread,
            [] {
                XC_LOGI("recv: logger 1 Hz (thread dedicato)");
                while (g_diagRun.load(std::memory_order_relaxed)) {
                    logRecvOnce();
                    for (int i = 0; i < 10 && g_diagRun.load(std::memory_order_relaxed); ++i)
                        platform::sleepMs(100);
                }
            },
            1u << 20)) {
        g_diagRun.store(0);
        XC_LOGW("recv: logger non partito");
    }
}

extern "C" void psbox_rd_stop_logger(void) {
    if (!g_diagRun.exchange(0)) return;
    g_diagThread.join();
}
