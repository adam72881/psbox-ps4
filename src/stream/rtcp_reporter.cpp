// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "stream/rtcp_reporter.h"

#include "stream/abr_plan.h"

#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace xc::stream {

namespace {

constexpr uint64_t kReportIntervalUs = 1000000;
constexpr uint64_t kPliQuietUs = 2000000;

uint64_t nowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

uint32_t get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

}  // namespace

RtcpReporter::RtcpReporter(uint32_t clockRate, uint32_t maxBitrate, int abrTier)
    : clockRate_(clockRate), maxBitrate_(maxBitrate) {
    int tier = abrTier;
    if (tier < ABR_720 || tier > ABR_1440) tier = ABR_1080;
    abr_ = bitrateBegin(tier, maxBitrate);
    estimate_ = abr_.currentBps;
}

void RtcpReporter::armPli() { pliArmed_.store(1, std::memory_order_relaxed); }

void RtcpReporter::onRtp(const uint8_t* d, size_t n, uint64_t now) {
    uint32_t ssrc = get32(d + 8);
    uint16_t seq = get16(d + 2);
    if (!started_ || ssrc != mediaSsrc_) {
        started_ = true;
        mediaSsrc_ = ssrc;
        baseSeq_ = seq;
        maxSeq_ = seq;
        cycles_ = 0;
        received_ = expectedPrior_ = receivedPrior_ = 0;
        haveTransit_ = false;
        jitter_ = 0;
    } else {
        uint16_t delta = static_cast<uint16_t>(seq - maxSeq_);
        if (delta < 0x8000) {
            if (seq < maxSeq_) cycles_ += 0x10000;
            maxSeq_ = seq;
        }
    }
    ++received_;
    bytesSinceReport_ += n;
    lastRtpUs_.store(now, std::memory_order_relaxed);

    uint32_t ts = get32(d + 4);
    if (haveTransit_ && ts == lastTimestamp_) return;
    int64_t arrival = static_cast<int64_t>(now * clockRate_ / 1000000u);
    int64_t transit = arrival - static_cast<int64_t>(ts);
    if (haveTransit_) {
        int64_t dt = transit - lastTransit_;
        if (dt < 0) dt = -dt;
        if (dt < static_cast<int64_t>(clockRate_)) jitter_ += (static_cast<double>(dt) - jitter_) / 16.0;
    }
    lastTransit_ = transit;
    lastTimestamp_ = ts;
    haveTransit_ = true;
}

void RtcpReporter::onRtcp(const uint8_t* d, size_t n, uint64_t now) {
    for (size_t off = 0; off + 4 <= n;) {
        uint8_t pt = d[off + 1];
        size_t len = (static_cast<size_t>(get16(d + off + 2)) + 1) * 4;
        if (off + len > n) break;
        if (pt == 200 && len >= 28) {
            lastSr_ = (get32(d + off + 8) << 16) | (get32(d + off + 12) >> 16);
            lastSrArrivalUs_ = now;
        }
        off += len;
    }
}

void RtcpReporter::sendPli(const rtc::message_callback& send) {
    if (!mediaSsrc_ || !send) return;
    std::vector<uint8_t> b;
    b.push_back(0x81);
    b.push_back(206);
    put16(b, 2);
    put32(b, kReceiverSsrc);
    put32(b, mediaSsrc_);
    auto* p = reinterpret_cast<const std::byte*>(b.data());
    try {
        send(rtc::make_message(p, p + b.size(), rtc::Message::Control));
    } catch (const std::exception& e) {
        XC_LOGW("rtcp: PLI %s", e.what());
    }
}

void RtcpReporter::report(uint64_t now, const rtc::message_callback& send) {
    uint64_t intervalUs = lastReportUs_ ? now - lastReportUs_ : kReportIntervalUs;
    lastReportUs_ = now;

    uint32_t extMax = cycles_ + maxSeq_;
    uint32_t expected = extMax - baseSeq_ + 1;
    int64_t lost = static_cast<int64_t>(expected) - received_;
    lost = std::clamp<int64_t>(lost, -0x800000, 0x7FFFFF);
    uint32_t expectedInterval = expected - expectedPrior_;
    uint32_t receivedInterval = received_ - receivedPrior_;
    expectedPrior_ = expected;
    receivedPrior_ = received_;
    int64_t lostInterval = static_cast<int64_t>(expectedInterval) - receivedInterval;
    uint8_t fraction = 0;
    if (expectedInterval > 0 && lostInterval > 0)
        fraction = static_cast<uint8_t>(std::min<int64_t>(255, (lostInterval << 8) / expectedInterval));
    uint32_t dlsr = 0;
    if (lastSrArrivalUs_) dlsr = static_cast<uint32_t>((now - lastSrArrivalUs_) * 65536 / 1000000);

    std::vector<uint8_t> b;
    b.reserve(32);
    b.push_back(0x81);
    b.push_back(201);
    put16(b, 7);
    put32(b, kReceiverSsrc);
    put32(b, mediaSsrc_);
    put32(b, (uint32_t(fraction) << 24) | (static_cast<uint32_t>(lost) & 0xFFFFFF));
    put32(b, extMax);
    put32(b, static_cast<uint32_t>(jitter_));
    put32(b, lastSr_);
    put32(b, lastSr_ ? dlsr : 0);

    uint32_t rate = intervalUs ? static_cast<uint32_t>(bytesSinceReport_ * 8 * 1000000 / intervalUs) : 0;
    receiveRate_ = rate;
    bytesSinceReport_ = 0;

    if (maxBitrate_ > 0) {
        int stallMs = 0;
        if (lastRtpUs_.load()) {
            uint64_t quiet = now > lastRtpUs_.load() ? now - lastRtpUs_.load() : 0;
            stallMs = static_cast<int>(quiet / 1000u);
        } else if (started_) {
            stallMs = kAbrStallMs;
        }
        uint32_t before = abr_.currentBps;
        bitrateOnSample(&abr_, rate, fraction, static_cast<int>(intervalUs / 1000), stallMs, 0);
        estimate_ = abr_.currentBps;
        if (bitrateDemoteDue(&abr_)) demoteWanted_.store(1, std::memory_order_relaxed);
        if (bitratePromoteDue(&abr_)) promoteWanted_.store(1, std::memory_order_relaxed);
        if (abr_.lastChanged) {
            XC_LOGI("abr: %u → %u kbps (%s) recv %u kbps stall %d ms", before / 1000u, abr_.currentBps / 1000u,
                    abr_.lastReason ? abr_.lastReason : "?", rate / 1000u, stallMs);
        }
        if (bitrateSendRemb(&abr_)) {
            std::vector<uint8_t> remb;
            sendRemb(abr_.currentBps, mediaSsrc_, remb);
            if (!remb.empty()) {
                auto* rp = reinterpret_cast<const std::byte*>(remb.data());
                send(rtc::make_message(rp, rp + remb.size(), rtc::Message::Control));
            }
        }
    }

    uint32_t n = ++reportsSent_;
    if (n <= 4 || n % 5 == 0 || abr_.lastChanged)
        XC_LOGI("rtcp: RR %u kbps ssrc %08x loss %u n %u%s", rate / 1000u, mediaSsrc_,
                static_cast<unsigned>(fraction), n, maxBitrate_ ? " REMB" : " audio");

    auto* p = reinterpret_cast<const std::byte*>(b.data());
    send(rtc::make_message(p, p + b.size(), rtc::Message::Control));
}

void RtcpReporter::sendRemb(uint32_t bps, uint32_t mediaSsrc, std::vector<uint8_t>& b) {
    uint8_t exp = 0;
    uint32_t mantissa = bps;
    while (mantissa >= (1u << 18)) {
        mantissa >>= 1;
        ++exp;
    }
    b.push_back(0x8F);
    b.push_back(206);
    put16(b, 5);
    put32(b, kReceiverSsrc);
    put32(b, 0);
    b.insert(b.end(), {'R', 'E', 'M', 'B'});
    put32(b, (1u << 24) | (uint32_t(exp) << 18) | mantissa);
    put32(b, mediaSsrc);
}

int RtcpReporter::secondsQuiet() const {
    uint64_t last = lastRtpUs_.load(std::memory_order_relaxed);
    if (!last) return 0;
    uint64_t now = nowUs();
    if (now <= last) return 0;
    return static_cast<int>((now - last) / 1000000u);
}

void RtcpReporter::pump() {
    rtc::message_callback send;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!send_) return;
        send = send_;
        uint64_t now = nowUs();
        if (maxBitrate_ > 0) {
            if (pliArmed_.exchange(0, std::memory_order_relaxed)) {
                sendPli(send);
                lastPliUs_ = now;
            } else if (started_ && lastRtpUs_.load() && now - lastRtpUs_.load() >= kPliQuietUs &&
                       (!lastPliUs_ || now - lastPliUs_ >= kPliQuietUs)) {
                sendPli(send);
                lastPliUs_ = now;
                XC_LOGI("rtcp: PLI video fermo");
            }
        }
        if (lastReportUs_ && now - lastReportUs_ < kReportIntervalUs) return;
        if (!started_) {
            lastReportUs_ = now;
            return;
        }
    }
    try {
        std::lock_guard<std::mutex> lock(mu_);
        uint64_t now = nowUs();
        if (now - lastReportUs_ < kReportIntervalUs) return;
        report(now, send);
    } catch (const std::exception& e) {
        XC_LOGW("rtcp: invio %s", e.what());
    }
}

void RtcpReporter::incoming(rtc::message_vector& messages, const rtc::message_callback& send) {
    std::lock_guard<std::mutex> lock(mu_);
    send_ = send;
    rtc::message_vector rtp;
    for (auto& message : messages) {
        const auto* d = reinterpret_cast<const uint8_t*>(message->data());
        size_t n = message->size();
        if (message->type == rtc::Message::Control) {
            onRtcp(d, n, nowUs());
        } else if (message->type == rtc::Message::Binary && n >= 12 && (d[0] >> 6) == 2) {
            onRtp(d, n, nowUs());
            rtp.push_back(std::move(message));
        }
    }
    messages.swap(rtp);
    if (maxBitrate_ > 0 && pliArmed_.exchange(0, std::memory_order_relaxed)) sendPli(send);
}

}  // namespace xc::stream
