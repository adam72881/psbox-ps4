* clockRate_ / 1000000u);
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
            else uniq.push_ba