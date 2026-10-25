/* SPDX-License-Identifier: GPL-3.0-or-later
 * Contatori permanenti del percorso di ricezione (xcloud.log, 1 Hz).
 */
#include "stream/recv_diag.h"

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
