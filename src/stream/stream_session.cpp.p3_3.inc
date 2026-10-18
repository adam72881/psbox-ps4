 (cb.audio) cb.audio(reinterpret_cast<const uint8_t*>(data.data()), data.size(), info.timestamp);
        });

        // Video: H.264 only, Main before Constrained Baseline / Baseline.
        rtc::Description::Video v("1", rtc::Description::Direction::RecvOnly);
        // Main profile at level 5.1 (0x33): the level that allows 1440p60; with
        // 3.1 the service tops out at 1080p (it answers level 4.2).
        v.addH264Codec(102, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d0033");
        v.addH264Codec(104, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
        v.addH264Codec(106, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f");
        // Come PS5 v0.9.8 e 01.35: transport-cc nell'offerta (ext id 3).
        // La 01.36 l'ha tolto «per allinearsi al REMB-only» ma sul PS5
        // l'offerta ha ancora transport-cc: senza, su Orbis il server non
        // risponde al ClientHello DTLS (0 byte RX → timeout).
        v.addExtMap(rtc::Description::Entry::ExtMap(
            3, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"));
        for (int pt : {102, 104, 106}) {
            if (auto* m = v.rtpMap(pt)) m->addFeedback("transport-cc");
        }
        v.setBitrate(static_cast<int>(opt.maxBitrate / 1000u));
        v.addSSRC(kReceiverSsrc, "video");
        XC_LOGI("video: offro %u kbps, ssrc rtcp %08x, transport-cc", opt.maxBitrate / 1000u, kReceiverSsrc);
        video = pc->addTrack(v);
        // Our jitter buffer (NACK, whole frames only) in place of
        // H264RtpDepacketizer, which hands over frames with holes in them.
        guard->self = this;
        auto videoReceiver = std::make_shared<H264Receiver>(
            [g = guard] {
                // Solo flag: DataChannel::send dal path RTP → deadlock SCTP/DTLS.
                std::lock_guard<std::mutex> lock(g->mutex);
                if (!g->self || !g->self->open) return;
               