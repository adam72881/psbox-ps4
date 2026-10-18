p RTP.
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
    int64_t arrival = static_cast<int64_t>(now 