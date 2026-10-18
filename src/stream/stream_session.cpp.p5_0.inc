 4 && decodeTeredo(parts[4], ipv4, port)) {
                XC_LOGI("teredo candidate -> port %d (and 9002)", port);
                teredoIpv4 = ipv4;
                teredoPort = port;
                addRemoteCandidate("a=candidate:10 1 UDP 1 " + ipv4 + " 9002 typ host", mid);
                addRemoteCandidate("a=candidate:11 1 UDP 1 " + ipv4 + " " + std::to_string(port) + " typ host", mid);
            }
            addRemoteCandidate(c.candidate, mid);
            remoteAdded.emplace_back(c.candidate, mid);
        }
        std::vector<int> answerPorts = mediaPortsFromSdp(answer);
        if (!teredoIpv4.empty() && !answerPorts.empty()) {
            XC_LOGI("dtls: porte m= answer %zu (fallback se 0 RX)", answerPorts.size());
            addHostPorts(teredoIpv4, answerPorts, bundleMid, 20);
        }

        XC_LOGI("webrtc: candidati remoti aggiunti");
        XC_LOGI("ice: attendo connected (20 s)");
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("ICE_WAIT");
#endif
        if (!waitFor([this] { return iceUp; }, 20000)) {
            std::lock_guard<std::mutex> lock(mutex);
            err = failed ? failReason : "ICE non connesso entro 20 s";
            XC_LOGE("ice: %s", err.c_str());
            return false;
        }
        XC_LOGI("ice: connected");
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("ICE_CONNECTED");
#endif

        // Attesa handshake fino a connectTimeoutMs. Niente riparto forzato a
        // 3 s (01.32 chiudeva la PeerConnection e riconnetteva: NEW_SEQUENCE
        // sul decoder e pad scollegato). A 3 s con DTLS 0 RX: re-iniezione
        // candidati (AJ-GFN: alcune porte rispondono ICE ma non DTLS).
        const int totalMs = opt.connectTimeoutMs > 0 ? opt.connectTimeoutMs : 30000;
        const uint64_t started = platform::nowMs();
#if defined(XCLOUD_PS4)
        bool reinjectedDtls = false;
#endif
        while (platform::nowMs() - started < static_cast<uint64_t>(totalMs)) {
            if (g_streamSto