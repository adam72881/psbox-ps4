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

// TWCC ogni ~50 ms (browser/PS5); RR ogni 1 s senza REMB se c'è transport-cc.
constexpr uint64_t kReportIntervalUs = 1000000;
constexpr uint64_t kTwccIntervalUs = 50000;
constexpr uint64_t kPliQuietUs = 2000000;
constexpr int64_t kDeltaScaleUs = 250;   // draft-holmer
constexpr int64_t kBaseScaleUs = 64000;  // 256 * 250 µs = 64 ms

uint64_t nowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

// extmap id negoziato nell'offerta (stream_session: id 3). One-byte e two-byte.
constexpr uint8_t kTwccExtId = 3;

bool readTwccSeq(const uint8_t* d, size_t n, uint16_t& out) {
    if (n < 12 || !(d[0] & 0x10)) return false;
    size_t off = 12 + 4u * (d[0] & 0x0f);
    if (off + 4 > n) return false;
    uint16_t profile = get16(d + off);
    size_t words = get16(d + off + 2);
    size_t end = off + 4 + words * 4;
    if (end > n) return false;
    size_t i = off + 4;
    if (profile == 0xBEDE) {
        while (i < end) {
            if (d[i] == 0) {
                ++i;
                continue;
            }
            uint8_t id = d[i] >> 4;
            uint8_t len = (d[i] & 0x0f) + 1;
            ++i;
            if (i + len > end) break;
            if (id == kTwccExtId && len >= 2) {
                out = get16(d + i);
                return true;
            }
            i += len;
        }
    } else if ((profile & 0xFFF0) == 0x1000) {
        while (i + 2 <= end) {
            if (d[i] == 0) {
                ++i;
                continue;
            }
            uint8_t id = d[i];
            uint8_t len = d[i + 1];
            i += 2;
            if (i + len > end) break;
            if (id == kTwccExtId && len >= 2) {
                out = get16(d + i);
                return true;
            }
            i += len;
        }
    }
    return false;
}

void logTwccPacket(uint32_t n, const std::vector<uint8_t>& b, uint16_t base, int statusCount) {
    XC_LOGI("[TWCC] size %zu base %u count %d n %u", b.size(), base, statusCount, n);
    if (n > 5) return;
    // Hex dump dei primi 5 feedback (byte-for-byte vs PS5/browser).
    char hex[768];
    size_t pos = 0;
    for (size_t i = 0; i < b.size() && pos + 3 < sizeof hex; ++i)
        pos += static_cast<size_t>(std::snprintf(hex + pos, sizeof hex - pos, "%02x", b[i]));
    XC_LOGI("[TWCC] hex#%u %s", n, hex);
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

    uint16_t tw = 0;
    if (readTwccSeq(d, n, tw)) {
        if (twccCount_ < kTwccCap) {
            int idx = (twccHead_ + twccCount_) % kTwccCap;
            twccHits_[idx] = TwccHit{tw, now};
            ++twccCount_;
        } else {
            twccHits_[twccHead_] = TwccHit{tw, now};
            twccHead_ = (twccHead_ + 1) % kTwccCap;
        }
    }

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

bool RtcpReporter::flushTwcc(uint64_t nowUsVal, const rtc::message_callback& send) {
    if (twccCount_ <= 0 || !send) return false;

    // Oldest-first: i hit più vecchi devono essere ACK (bug 01.38: newest-only
    // → GCC spegneva a ~57 feedback / ~6–8 s).
    std::vector<TwccHit> hits;
    hits.reserve(static_cast<size_t>(twccCount_));
    for (int i = 0; i < twccCount_; ++i) hits.push_back(twccHits_[(twccHead_ + i) % kTwccCap]);

    std::sort(hits.begin(), hits.end(),
              [](const TwccHit& a, const TwccHit& b) { return static_cast<int16_t>(a.seq - b.seq) < 0; });
    {
        std::vector<TwccHit> uniq;
        for (const auto& h : hits) {
            if (!uniq.empty() && uniq.back().seq == h.seq) uniq.back() = h;
            else uniq.push_back(h);
        }
        hits.swap(uniq);
    }
    if (hits.empty()) {
        XC_LOGW("[TWCC] skip: nessun hit dopo uniq (ring %d)", twccCount_);
        twccHead_ = 0;
        twccCount_ = 0;
        return false;
    }

    // Cap ragionevole per MTU: ~100 status (WebRTC max ~0xffff ma Orbis MTU 1200).
    constexpr int kMaxHits = 80;
    constexpr int kMaxSpan = 120;
    size_t take = hits.size();
    if (static_cast<int>(take) > kMaxHits) take = static_cast<size_t>(kMaxHits);
    while (take > 1) {
        int span = static_cast<int>(static_cast<uint16_t>(hits[take - 1].seq - hits[0].seq)) + 1;
        if (span <= kMaxSpan) break;
        --take;
    }
    if (take < hits.size()) {
        static uint32_t trimLog = 0;
        if (++trimLog <= 4 || trimLog % 50 == 0)
            XC_LOGI("[TWCC] backlog %zu → invio %zu oldest (resto in coda)", hits.size(), take);
        hits.resize(take);
    }

    uint16_t base = hits.front().seq;
    uint16_t last = hits.back().seq;
    int statusCount = static_cast<int>(static_cast<uint16_t>(last - base)) + 1;
    if (statusCount > kMaxSpan || statusCount > static_cast<int>(hits.size()) * 4) {
        XC_LOGW("[TWCC] skip range: base %u last %u span %d hits %zu — invio solo il primo", base, last,
                statusCount, hits.size());
        hits.resize(1);
        last = base;
        statusCount = 1;
    }
    if (statusCount > 0x1FFF) statusCount = 0x1FFF;
    if (statusCount <= 0) {
        XC_LOGW("[TWCC] skip: statusCount %d", statusCount);
        return false;
    }

    std::vector<uint64_t> arr(static_cast<size_t>(statusCount), 0);
    for (const auto& h : hits) {
        int idx = static_cast<uint16_t>(h.seq - base);
        if (idx >= 0 && idx < statusCount) arr[static_cast<size_t>(idx)] = h.us;
    }

    // Reference time: multipli di 64 ms, 24 bit (come WebRTC). I delta interni
    // usano il base a piena precisione — mai ref24*64000 (perde i bit alti).
    const int64_t firstUs = static_cast<int64_t>(hits.front().us);
    const int64_t baseTimeUs = (firstUs / kBaseScaleUs) * kBaseScaleUs;
    const uint32_t ref24 = static_cast<uint32_t>((baseTimeUs / kBaseScaleUs) & 0x00FFFFFFu);
    int64_t prevUs = baseTimeUs;

    std::vector<uint16_t> chunks;
    std::vector<uint8_t> deltas;
    int i = 0;
    while (i < statusCount) {
        bool recv = arr[static_cast<size_t>(i)] != 0;
        if (!recv) {
            int run = 1;
            while (i + run < statusCount && run < 0x1FFF && arr[static_cast<size_t>(i + run)] == 0) ++run;
            // Run-length T=0 S=0: not received.
            chunks.push_back(static_cast<uint16_t>(run & 0x1FFF));
            i += run;
            continue;
        }
        int64_t us = static_cast<int64_t>(arr[static_cast<size_t>(i)]);
        int64_t dUs = us - prevUs;
        prevUs = us;
        // Arrotonda a 250 µs come WebRTC (half-up verso lontano da zero).
        int64_t ticks = dUs >= 0 ? (dUs + kDeltaScaleUs / 2) / kDeltaScaleUs
                                 : (dUs - kDeltaScaleUs / 2) / kDeltaScaleUs;
        // Symbol 1 = small delta: 8-bit UNSIGNED [0..255] (mai signed!).
        // Symbol 2 = large/negative: 16-bit signed.
        if (ticks >= 0 && ticks <= 255) {
            chunks.push_back(static_cast<uint16_t>((1u << 13) | 1));
            deltas.push_back(static_cast<uint8_t>(ticks));
        } else {
            if (ticks > 0x7FFF) ticks = 0x7FFF;
            if (ticks < -0x8000) ticks = -0x8000;
            chunks.push_back(static_cast<uint16_t>((2u << 13) | 1));
            auto v = static_cast<uint16_t>(static_cast<int16_t>(ticks));
            deltas.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
            deltas.push_back(static_cast<uint8_t>(v & 0xff));
        }
        ++i;
    }

    std::vector<uint8_t> b;
    b.reserve(32 + chunks.size() * 2 + deltas.size() + 4);
    b.push_back(0x8F);  // V=2 P=0 FMT=15
    b.push_back(205);   // PT=RTPFB
    put16(b, 0);
    put32(b, kReceiverSsrc);  // sender SSRC
    put32(b, mediaSsrc_);     // media SSRC = video SSRC
    put16(b, base);
    put16(b, static_cast<uint16_t>(statusCount));
    b.push_back(static_cast<uint8_t>((ref24 >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((ref24 >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>(ref24 & 0xff));
    b.push_back(twccFbCount_);
    for (uint16_t c : chunks) put16(b, c);
    b.insert(b.end(), deltas.begin(), deltas.end());
    while (b.size() % 4) b.push_back(0);
    size_t words = b.size() / 4 - 1;
    b[2] = static_cast<uint8_t>((words >> 8) & 0xff);
    b[3] = static_cast<uint8_t>(words & 0xff);

    auto* p = reinterpret_cast<const std::byte*>(b.data());
    try {
        send(rtc::make_message(p, p + b.size(), rtc::Message::Control));
    } catch (const std::exception& e) {
        XC_LOGW("[TWCC] send fallito: %s (ring intatto, count %d)", e.what(), twccCount_);
        return false;
    }

    {
        std::vector<TwccHit> rest;
        rest.reserve(static_cast<size_t>(twccCount_));
        for (int j = 0; j < twccCount_; ++j) {
            TwccHit h = twccHits_[(twccHead_ + j) % kTwccCap];
            int d = static_cast<uint16_t>(h.seq - base);
            if (d >= 0 && d < statusCount) continue;
            rest.push_back(h);
        }
        twccHead_ = 0;
        twccCount_ = static_cast<int>(rest.size());
        if (twccCount_ > kTwccCap) twccCount_ = kTwccCap;
        for (int j = 0; j < twccCount_; ++j) twccHits_[j] = rest[static_cast<size_t>(j)];
    }
    ++twccFbCount_;
    uint32_t n = ++twccSent_;
    lastTwccUs_ = nowUsVal;
    twccActive_ = true;
    logTwccPacket(n, b, base, statusCount);
    return true;
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

    // RR + TWCC + REMB. Il REMB porta la stima ABR (anche al tetto): senza,
    // con solo TWCC la stima restava bloccata a bitrate_ask (01.51).
    uint32_t rate = intervalUs ? static_cast<uint32_t>(bytesSinceReport_ * 8 * 1000000 / intervalUs) : 0;
    receiveRate_ = rate;
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
    bool sentRemb = bitrateSendRemb(&abr_) != 0;
    if (sentRemb) {
        std::vector<uint8_t> remb;
        sendRemb(abr_.currentBps, mediaSsrc_, remb);
        if (!remb.empty()) {
            auto* rp = reinterpret_cast<const std::byte*>(remb.data());
            send(rtc::make_message(rp, rp + remb.size(), rtc::Message::Control));
        }
    }
    uint32_t n = ++reportsSent_;
    if (n <= 4 || n % 5 == 0 || abr_.lastChanged)
        XC_LOGI("rtcp: RR %u kbps (recv) stima %u kbps ssrc %08x loss %u n %u twcc %u%s", rate / 1000u,
                abr_.currentBps / 1000u, mediaSsrc_, static_cast<unsigned>(fraction), n, twccSent_.load(),
                sentRemb ? " REMB" : "");
    bytesSinceReport_ = 0;

    auto* p = reinterpret_cast<const std::byte*>(b.data());
    send(rtc::make_message(p, p + b.size(), rtc::Message::Control));
    if (twccCount_ > 0 && (now - lastTwccUs_ >= kTwccIntervalUs || twccCount_ >= 40)) {
        for (int k = 0; k < 8 && twccCount_ > 0; ++k) {
            if (!flushTwcc(now, send)) break;
        }
    }
}

void RtcpReporter::sendRemb(uint32_t bps, uint32_t mediaSsrc, std::vector<uint8_t>& b) {
    // Tetto ABR verso il server; va con TWCC.
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
        bool due = twccCount_ > 0 && (!lastTwccUs_ || now - lastTwccUs_ >= kTwccIntervalUs || twccCount_ >= 40);
        if (due) {
            for (int n = 0; n < 8 && twccCount_ > 0; ++n) {
                if (!flushTwcc(now, send)) break;
                now = nowUs();
            }
        }
        if (pliArmed_.exchange(0, std::memory_order_relaxed)) {
            sendPli(send);
            lastPliUs_ = now;
        } else if (started_ && maxBitrate_ && lastRtpUs_.load() && now - lastRtpUs_.load() >= kPliQuietUs &&
                   (!lastPliUs_ || now - lastPliUs_ >= kPliQuietUs)) {
            sendPli(send);
            lastPliUs_ = now;
            XC_LOGI("rtcp: PLI video fermo");
        }
        /* REMB è nel report() (anche con TWCC): senza, la stima resta a ask. */
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
            // Timestamp per pacchetto (orologio monotono): non un solo now per batch.
            onRtp(d, n, nowUs());
            rtp.push_back(std::move(message));
        }
    }
    messages.swap(rtp);
    if (pliArmed_.exchange(0, std::memory_order_relaxed)) sendPli(send);
}

}  // namespace xc::stream
