// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "stream/rtcp_reporter.h"

#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace xc::stream {

namespace {

constexpr uint64_t kReportIntervalUs = 500000;  // 500 ms come PS5 REMB
constexpr uint64_t kTwccIntervalUs = 100000;    // 100 ms
constexpr uint64_t kPliQuietUs = 2000000;       // PLI se video fermo ≥2 s
constexpr uint32_t kMinBitrate = 1500000;

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
            // Solo id 3: quello negoziato nell'offerta. Id 2/4 potevano
            // matchare altre estensioni → seq spurie → "loss" TWCC → stop RTP.
            if (id == 3 && len >= 2) {
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
            if (id == 3 && len >= 2) {
                out = get16(d + i);
                return true;
            }
            i += len;
        }
    }
    return false;
}

}  // namespace

RtcpReporter::RtcpReporter(uint32_t clockRate, uint32_t maxBitrate)
    : clockRate_(clockRate), maxBitrate_(maxBitrate) {
    estimate_ = maxBitrate;
}

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

bool RtcpReporter::flushTwcc(uint64_t nowUs, const rtc::message_callback& send) {
    if (twccCount_ <= 0 || !send) return false;

    // Copia senza svuotare: si azzera solo dopo send ok.
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
    if (hits.empty()) return false;

    // Cap per feedback: in 01.35 count 222/314 → UDP ~MTU e "loss" enorme.
    // Max 40 hit (≥100 ms a ~400 pkt/s) resta sotto ~400 B con mtu 1200.
    constexpr int kMaxHits = 40;
    if (static_cast<int>(hits.size()) > kMaxHits)
        hits.erase(hits.begin(), hits.end() - kMaxHits);

    uint16_t base = hits.front().seq;
    uint16_t last = hits.back().seq;
    int statusCount = static_cast<int>(static_cast<uint16_t>(last - base)) + 1;
    // Range troppo largo rispetto ai pacchetti ricevuti = buco fittizio da
    // seq parse errato (ext id 2/4 in 01.35): il server spegne l'RTP.
    if (statusCount > static_cast<int>(hits.size()) * 4 || statusCount > 80) {
        TwccHit keep = hits.back();
        hits.clear();
        hits.push_back(keep);
        base = keep.seq;
        last = base;
        statusCount = 1;
    }
    if (statusCount > 0x1FFF) statusCount = 0x1FFF;
    if (statusCount <= 0) return false;

    std::vector<uint64_t> arr(static_cast<size_t>(statusCount), 0);
    for (const auto& h : hits) {
        int idx = static_cast<uint16_t>(h.seq - base);
        if (idx >= 0 && idx < statusCount) arr[static_cast<size_t>(idx)] = h.us;
    }

    // reference time: 24 bit × 64 ms, allineato al primo arrivo (delta≈0).
    uint64_t firstUs = hits.front().us;
    uint32_t refMs64 = static_cast<uint32_t>((firstUs / 1000u) / 64u) & 0x00FFFFFFu;
    uint64_t prevUs = static_cast<uint64_t>(refMs64) * 64000ull;

    std::vector<uint16_t> chunks;
    std::vector<uint8_t> deltas;
    int i = 0;
    while (i < statusCount) {
        bool recv = arr[static_cast<size_t>(i)] != 0;
        int run = 1;
        // Per i ricevuti non si usa un unico run se servono large delta misti:
        // status 1 = small (1 byte), 2 = large (2 byte). Si spezza per tipo.
        if (!recv) {
            while (i + run < statusCount && run < 0x1FFF && arr[static_cast<size_t>(i + run)] == 0) ++run;
            chunks.push_back(static_cast<uint16_t>(run & 0x1FFF));  // T=0 S=0
            i += run;
            continue;
        }
        // Un pacchetto alla volta per scegliere small/large correttamente.
        uint64_t us = arr[static_cast<size_t>(i)];
        int64_t dUs = static_cast<int64_t>(us) - static_cast<int64_t>(prevUs);
        prevUs = us;
        int64_t ticks = dUs / 250;
        if (ticks >= -128 && ticks <= 127) {
            chunks.push_back(static_cast<uint16_t>((1u << 13) | 1));  // S=1 run=1
            deltas.push_back(static_cast<uint8_t>(static_cast<int8_t>(ticks)));
        } else {
            if (ticks > 0x7FFF) ticks = 0x7FFF;
            if (ticks < -0x8000) ticks = -0x8000;
            chunks.push_back(static_cast<uint16_t>((2u << 13) | 1));  // S=2 large
            auto v = static_cast<uint16_t>(static_cast<int16_t>(ticks));
            deltas.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
            deltas.push_back(static_cast<uint8_t>(v & 0xff));
        }
        ++i;
    }

    std::vector<uint8_t> b;
    b.reserve(32 + chunks.size() * 2 + deltas.size() + 4);
    b.push_back(0x8F);
    b.push_back(205);
    put16(b, 0);
    put32(b, kReceiverSsrc);
    put32(b, mediaSsrc_);
    put16(b, base);
    put16(b, static_cast<uint16_t>(statusCount));
    b.push_back(static_cast<uint8_t>((refMs64 >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((refMs64 >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>(refMs64 & 0xff));
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
        XC_LOGW("rtcp: twcc %s", e.what());
        return false;  // ring intatto per il prossimo tentativo
    }
    // Send ok: svuota e avanza il contatore feedback.
    twccHead_ = 0;
    twccCount_ = 0;
    ++twccFbCount_;
    uint32_t n = ++twccSent_;
    lastTwccUs_ = nowUs;
    if (n <= 8 || n % 20 == 0)
        XC_LOGI("rtcp: TWCC base %u count %d chunks %zu n %u", base, statusCount, chunks.size(), n);
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
    b.reserve(64);
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

    if (maxBitrate_) {
        uint32_t rate = intervalUs ? static_cast<uint32_t>(bytesSinceReport_ * 8 * 1000000 / intervalUs) : 0;
        receiveRate_ = rate;
        double loss = fraction / 256.0;
        uint32_t est = estimate_;
        if (loss > 0.10) {
            if (++lossyReports_ >= 3)
                est = std::max<uint32_t>(kMinBitrate, static_cast<uint32_t>(rate * (1.0 - 0.5 * loss)));
        } else {
            lossyReports_ = 0;
            if (loss < 0.02)
                est = std::min<uint32_t>(maxBitrate_, std::max<uint32_t>(est, kMinBitrate) + maxBitrate_ / 20);
        }
        estimate_ = est;
        sendRemb(est, mediaSsrc_, b);
        uint32_t n = ++reportsSent_;
        if (n <= 4 || n % 5 == 0)
            XC_LOGI("rtcp: RR+REMB %u bps (log %u kbps) ssrc %08x loss %u n %u twcc %u", est, est / 1000u, mediaSsrc_,
                    static_cast<unsigned>(fraction), n, twccSent_.load());
    } else {
        ++reportsSent_;
    }
    bytesSinceReport_ = 0;

    auto* p = reinterpret_cast<const std::byte*>(b.data());
    send(rtc::make_message(p, p + b.size(), rtc::Message::Control));
    if (now - lastTwccUs_ >= kTwccIntervalUs) flushTwcc(now, send);
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
    if (reportsSent_.load() < 2)
        XC_LOGI("rtcp: REMB mantissa %u exp %u = %u bps", mantissa, exp, mantissa << exp);
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
        if (twccCount_ > 0 && (!lastTwccUs_ || now - lastTwccUs_ >= kTwccIntervalUs)) flushTwcc(now, send);
        // Video fermo: PLI RTCP (come 01.24) con backoff 2 s, non ogni RR.
        if (started_ && maxBitrate_ && lastRtpUs_.load() && now - lastRtpUs_.load() >= kPliQuietUs &&
            (!lastPliUs_ || now - lastPliUs_ >= kPliQuietUs)) {
            sendPli(send);
            lastPliUs_ = now;
            XC_LOGI("rtcp: PLI video fermo");
        }
        if (lastReportUs_ && now - lastReportUs_ < kReportIntervalUs) return;
        if (!started_) {
            if (!maxBitrate_) return;
            std::vector<uint8_t> b;
            estimate_ = maxBitrate_;
            sendRemb(maxBitrate_, 0, b);
            lastReportUs_ = now;
            uint32_t n = ++reportsSent_;
            XC_LOGI("rtcp: REMB iniziale %u bps n %u", maxBitrate_, n);
            auto* p = reinterpret_cast<const std::byte*>(b.data());
            try {
                send(rtc::make_message(p, p + b.size(), rtc::Message::Control));
            } catch (const std::exception& e) {
                XC_LOGW("rtcp: invio %s", e.what());
            }
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
    uint64_t now = nowUs();
    rtc::message_vector rtp;
    for (auto& message : messages) {
        const auto* d = reinterpret_cast<const uint8_t*>(message->data());
        size_t n = message->size();
        if (message->type == rtc::Message::Control) {
            onRtcp(d, n, now);
        } else if (message->type == rtc::Message::Binary && n >= 12 && (d[0] >> 6) == 2) {
            onRtp(d, n, now);
            rtp.push_back(std::move(message));
        }
    }
    messages.swap(rtp);
    // Solo accumulo qui: TWCC/REMB partono da pump() sul thread app.
    // Mandare RTCP dal path RTP (con mu_ tenuto) su Orbis bloccava il recv
    // DTLS/ICE e contribuiva a feedback di congestione spurii.
}

}  // namespace xc::stream
