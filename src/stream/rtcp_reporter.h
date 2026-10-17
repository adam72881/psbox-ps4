// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// RTCP: RR (RFC 3550), REMB, transport-cc (draft-holmer) ogni ~100 ms,
// PLI se il video tace (senza spam all'avvio).
#pragma once

#include <rtc/rtc.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace xc::stream {

constexpr uint32_t kReceiverSsrc = 0x50534278;  // "PSBx"

class RtcpReporter final : public rtc::MediaHandler {
public:
    RtcpReporter(uint32_t clockRate, uint32_t maxBitrate);

    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;
    void pump();
    int secondsQuiet() const;

    uint32_t estimate() const { return estimate_; }
    uint32_t receiveRate() const { return receiveRate_; }
    uint32_t reportsSent() const { return reportsSent_.load(); }
    uint32_t twccSent() const { return twccSent_.load(); }

private:
    void onRtp(const uint8_t* d, size_t n, uint64_t nowUs);
    void onRtcp(const uint8_t* d, size_t n, uint64_t nowUs);
    void report(uint64_t nowUs, const rtc::message_callback& send);
    void sendRemb(uint32_t bps, uint32_t mediaSsrc, std::vector<uint8_t>& out);
    void sendPli(const rtc::message_callback& send);
    bool flushTwcc(uint64_t nowUs, const rtc::message_callback& send);

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
    uint64_t lastTwccUs_ = 0;
    uint64_t lastPliUs_ = 0;
    uint64_t bytesSinceReport_ = 0;
    std::atomic<uint64_t> lastRtpUs_{0};
    int lossyReports_ = 0;
    std::atomic<uint32_t> estimate_{0}, receiveRate_{0}, reportsSent_{0}, twccSent_{0};

    static constexpr int kTwccCap = 512;
    struct TwccHit {
        uint16_t seq = 0;
        uint64_t us = 0;
    };
    TwccHit twccHits_[kTwccCap]{};
    int twccHead_ = 0;
    int twccCount_ = 0;
    uint8_t twccFbCount_ = 0;

    rtc::message_callback send_;
    std::mutex mu_;
};

}  // namespace xc::stream
