// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// RTCP come XCloud4/Nexo: RR (RFC 3550) + REMB + PLI. Niente transport-cc /
// TWCC (01.38/01.52: lo stream si fermava a ~5–8 s / ~109 feedback).
#pragma once

#include "stream/abr_plan.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace xc::stream {

constexpr uint32_t kReceiverSsrc = 0x50534278;  // "PSBx"

class RtcpReporter final : public rtc::MediaHandler {
public:
    RtcpReporter(uint32_t clockRate, uint32_t maxBitrate, int abrTier = ABR_1080);

    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;
    void pump();
    void armPli();
    int secondsQuiet() const;

    uint32_t estimate() const { return estimate_; }
    uint32_t receiveRate() const { return receiveRate_; }
    uint32_t reportsSent() const { return reportsSent_.load(); }
    uint32_t twccSent() const { return 0; }
    bool demoteWanted() const { return demoteWanted_.load(std::memory_order_relaxed) != 0; }
    bool promoteWanted() const { return promoteWanted_.load(std::memory_order_relaxed) != 0; }
    void clearTierHints() {
        demoteWanted_.store(0, std::memory_order_relaxed);
        promoteWanted_.store(0, std::memory_order_relaxed);
    }

private:
    void onRtp(const uint8_t* d, size_t n, uint64_t nowUs);
    void onRtcp(const uint8_t* d, size_t n, uint64_t nowUs);
    void report(uint64_t nowUs, const rtc::message_callback& send);
    void sendRemb(uint32_t bps, uint32_t mediaSsrc, std::vector<uint8_t>& out);
    void sendPli(const rtc::message_callback& send);

    const uint32_t clockRate_, maxBitrate_;
    uint32_t mediaSsrc_ = 0;
    bool started_ = false;
    uint16_t maxSeq_ = 0;
    uint32_t cycles_ = 0, baseSeq_ = 0;
    uint32_t received_ = 0, expectedPrior_ = 0, receivedPrior_ = 0;
    double jitter_ = 0;
    int64_t lastTransit_ = 0;
    uint32_t lastTimestamp_ = 0;
    bool haveTransit_ = false;
    uint32_t lastSr_ = 0;
    uint64_t lastSrArrivalUs_ = 0;
    uint64_t lastReportUs_ = 0;
    uint64_t lastPliUs_ = 0;
    uint64_t bytesSinceReport_ = 0;
    std::atomic<uint64_t> lastRtpUs_{0};
    BitratePlan abr_{};
    std::atomic<int> pliArmed_{0};
    std::atomic<int> demoteWanted_{0};
    std::atomic<int> promoteWanted_{0};
    std::atomic<uint32_t> estimate_{0}, receiveRate_{0}, reportsSent_{0};

    rtc::message_callback send_;
    std::mutex mu_;
};

}  // namespace xc::stream
