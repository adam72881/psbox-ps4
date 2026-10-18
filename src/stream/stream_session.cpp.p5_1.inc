p.load()) {
                err = "chiusura";
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (failed) {
                    err = failReason;
                    return false;
                }
                if (handshakeDone) {
#if defined(XCLOUD_PS4)
                    ::xc::app::trace::setStage("DTLS_OK");
#endif
                    return true;
                }
            }
#if defined(XCLOUD_PS4)
            if (!reinjectedDtls && platform::nowMs() - started >= 3000 && xc_dtls_rx_pkts() == 0) {
                reinjectedDtls = true;
                XC_LOGW("dtls: 0 byte a 3 s dopo ICE — re-iniezione candidati (niente riparto PC)");
                ::xc::app::trace::setStage("DTLS_REINJECT");
                for (const auto& rc : remoteAdded) addRemoteCandidate(rc.first, rc.second);
                if (!teredoIpv4.empty()) {
                    std::vector<int> ports = answerPorts;
                    ports.push_back(9002);
                    if (teredoPort > 0) ports.push_back(teredoPort);
                    addHostPorts(teredoIpv4, ports, bundleMid, 40);
                }
            }
#endif
            if (waitFor([this] { return handshakeDone; }, 200)) {
#if defined(XCLOUD_PS4)
                ::xc::app::trace::setStage("DTLS_OK");
#endif
                return true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            err = failed ? failReason : "timed out connecting the stream";
        }
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("DTLS_TIMEOUT");
#endif
        return false;
    }

    bool start(std::string& err) {
        // Un solo tentativo: il riparto ICE/DTLS a 3 s della 01.27–01.32
        // chiudeva la PC mentre il server era già connesso.
        std::string localErr;
        if (negotiateOnce(localErr)) {
            lastKeepaliveMs = lastKeyframeMs = platform::nowMs();
            return t