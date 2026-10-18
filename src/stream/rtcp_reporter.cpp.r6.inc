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
        if