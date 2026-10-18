 twcc %s", e.what());
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
      