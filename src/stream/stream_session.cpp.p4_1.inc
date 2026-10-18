}
    }

    void addRemoteCandidate(const std::string& candidate, const std::string& mid) {
        std::string c = candidate;
        while (!c.empty() && (c.back() == ' ' || c.back() == '\r' || c.back() == '\n')) c.pop_back();
        try {
            pc->addRemoteCandidate(rtc::Candidate(c, mid));
            XC_LOGD("remote candidate added");  // not its address
        } catch (const std::exception& e) {
            XC_LOGW("ignoring a remote candidate: %s", e.what());
        }
    }

    void resetPeerForRetry() {
        try {
            if (pc) pc->close();
        } catch (...) {
        }
        pc.reset();
        chat.reset();
        control.reset();
        input.reset();
        message.reset();
        audio.reset();
        video.reset();
        videoReporter.reset();
        {
            std::lock_guard<std::mutex> lock(mutex);
            localCandidates.clear();
            gatheringDone = false;
            handshakeDone = false;
            iceUp = false;
            failed = false;
            failReason.clear();
        }
#if defined(XCLOUD_PS4)
        xc_dtls_rx_reset();
#endif
    }

    // Una negoziazione ICE+SDP. Dopo ICE connected si attende l'handshake
    // fino a connectTimeoutMs (niente chiusura PC a 3 s).
    bool negotiateOnce(std::string& err) {
        createPeer();
        if (!pc) {
            err = "PeerConnection nulla";
            return false;
        }
#if defined(XCLOUD_PS4)
        xc_dtls_rx_reset();
#endif
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("ICE_LOCAL");
#endif
        XC_LOGI("webrtc: setLocalDescription ICE");
        pc->setLocalDescription(rtc::Description::Type::Offer);
        if (!waitFor([this] { return gatheringDone; }, 10000))
            XC_LOGW("ICE gathering did not complete; continuing with %zu candidates", localCandidates.size());
        XC_LOGI("webrtc: ICE %s, %zu candidati", gatheringDone ? "completo" : "incompleto",
                localCandidates.size());

      