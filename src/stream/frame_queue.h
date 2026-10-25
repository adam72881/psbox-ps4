/* SPDX-License-Identifier: GPL-3.0-or-later
 * Coda SPSC lock-free per frame video: il thread RTP fa push (drop se piena),
 * il thread decoder fa pop. Niente mutex nei callback di libdatachannel.
 */
#ifndef PSBOX_FRAME_QUEUE_H
#define PSBOX_FRAME_QUEUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace xc::stream {

struct QueuedFrame {
    std::vector<uint8_t> data;
    uint32_t rtpTimestamp = 0;
    double arrivalMs = 0;
    uint64_t arrivalUs = 0;
};

template <size_t Capacity>
class FrameQueue {
public:
    bool tryPush(QueuedFrame&& f) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t next = (t + 1) % Capacity;
        if (next == head_.load(std::memory_order_acquire)) return false;  // full
        slots_[t] = std::move(f);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(QueuedFrame& out) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;  // empty
        out = std::move(slots_[h]);
        head_.store((h + 1) % Capacity, std::memory_order_release);
        return true;
    }

    size_t sizeApprox() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return t >= h ? t - h : Capacity - h + t;
    }

    void clear() {
        QueuedFrame sink;
        while (tryPop(sink)) {
        }
    }

private:
    QueuedFrame slots_[Capacity];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

}  // namespace xc::stream

#endif
