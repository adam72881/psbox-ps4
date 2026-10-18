«connessione persa».
            if (std::strcmp(label, "chat") == 0 || std::strcmp(label, "message") == 0) return;
            if (open) fail(std::string("data channel ") + label + " closed");
        });
        return ch;
    }

    void createPeer() {
        XC_LOGI("webrtc: logger");
        initRtcLogging();
        XC_LOGI("webrtc: config");
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        // Default libdatachannel = 1280. Su Orbis i TWCC grandi della 01.35
        // (count 222–314 ≈ 900–1300 B) finivano al bordo MTU; i ClientHello
        // DTLS sono ~342 B quindi il fallimento 01.36 non era MTU, ma i
        // feedback grandi sì. 1200 lascia margine su Teredo/UDP.
        config.mtu = 1200;
        /* STUN di Teams: il primo server è l'unico che libjuice interroga.
         * Senza, l'unico candidato locale è l'host di getifaddrs. */
        config.iceServers.emplace_back("worldaz.relay.teams.microsoft.com", uint16_t{3478});
        XC_LOGI("webrtc: ctor (mtu %zu)", *config.mtu);
        pc = std::make_shared<rtc::PeerConnection>(config);
        XC_LOGI("webrtc: ctor ok");

        pc->onLocalCandidate([this](rtc::Candidate c) {
            const char* kind = "altro";
            switch (c.type()) {
            case rtc::Candidate::Type::Host: kind = "host"; break;
            case rtc::Candidate::Type::ServerReflexive: kind = "srflx"; break;
            case rtc::Candidate::Type::Relayed: kind = "relay"; break;
            case rtc::Candidate::Type::PeerReflexive: kind = "prflx"; break;
            default: break;
            }
            XC_LOGI("ice: candidato locale %s", kind);
            std::lock_guard<std::mutex> lock(mutex);
            localCandidates.push_back(std::move(c));
        });
        pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
            if (s == rtc::PeerConnection::GatheringState::Complete) {
                {
                    std::lock_guard<std::mutex>