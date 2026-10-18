 (twccCount_ > 0 && (!lastTwccUs_ || now - lastTwccUs_ >= kTwccIntervalUs)) flushTwcc(now, send);
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
            onRtp(d