 %zu local ICE candidates", mine.size());
        if (!gssv.sendIceCandidates(mine, err)) return false;

        std::vector<xcloud::IceCandidate> theirs;
        for (int i = 0; i < 30 && theirs.empty(); ++i) {
            if (!gssv.pollIceCandidates(theirs, err)) return false;
            if (theirs.empty()) platform::sleepMs(1000);
        }
        if (theirs.empty()) {
            err = "the server sent no ICE candidates";
            return false;
        }
        std::vector<std::string> remoteMids = midsInOrder(answer);
        std::string bundleMid = remoteMids.empty() ? "0" : remoteMids.front();
        // Per re-iniezione se ICE ok ma DTLS 0 RX (AJ-GFN delayed-manual-candidates).
        std::string teredoIpv4;
        int teredoPort = 0;
        std::vector<std::pair<std::string, std::string>> remoteAdded;  // candidate, mid
        auto addHostPorts = [this](const std::string& ipv4, const std::vector<int>& ports, const std::string& mid,
                                   int foundationBase) {
            int f = foundationBase;
            for (int port : ports) {
                if (port <= 0) continue;
                addRemoteCandidate("a=candidate:" + std::to_string(f++) + " 1 UDP 1 " + ipv4 + " " +
                                       std::to_string(port) + " typ host",
                                   mid);
            }
        };
        for (const auto& c : theirs) {
            if (c.candidate.find("end-of-candidates") != std::string::npos) continue;
            std::string mid = c.sdpMid.empty() ? bundleMid : c.sdpMid;
            std::vector<std::string> parts;
            size_t p = 0;
            while (p < c.candidate.size()) {
                size_t e = c.candidate.find(' ', p);
                if (e == std::string::npos) e = c.candidate.size();
                parts.push_back(c.candidate.substr(p, e - p));
                p = e + 1;
            }
            std::string ipv4;
            int port = 0;
            if (parts.size() >