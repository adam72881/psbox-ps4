  auto local = pc->localDescription();
        if (!local) {
            err = "no local SDP offer";
            return false;
        }
        std::string offer = std::string(*local);
        XC_LOGD("local offer:\n%s", offer.c_str());
        if (!gssv.sendSdpOffer(offer, err)) return false;

        std::string answer;
        for (int i = 0; i < 60 && answer.empty(); ++i) {
            if (g_streamStop.load()) {
                err = "chiusura";
                return false;
            }
            if (!gssv.pollSdpAnswer(answer, err)) return false;
            if (answer.empty()) platform::sleepMs(500);
        }
        if (answer.empty()) {
            err = "timed out waiting for the SDP answer";
            return false;
        }
        XC_LOGD("remote answer:\n%s", answer.c_str());
        for (size_t p = answer.find("a=rtpmap:"); p != std::string::npos; p = answer.find("a=rtpmap:", p + 1))
            XC_LOGI("answer %s", answer.substr(p, answer.find_first_of("\r\n", p) - p).c_str());
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("DTLS_REMOTE_SDP");
#endif
        XC_LOGI("webrtc: setRemoteDescription DTLS");
        try {
            pc->setRemoteDescription(rtc::Description(answer, rtc::Description::Type::Answer));
        } catch (const std::exception& e) {
            err = std::string("remote SDP rejected: ") + e.what();
            return false;
        }

        std::vector<std::string> mids = midsInOrder(offer);
        std::vector<xcloud::IceCandidate> mine;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& c : localCandidates) {
                xcloud::IceCandidate ic;
                ic.candidate = std::string(c);
                ic.sdpMid = c.mid();
                for (size_t i = 0; i < mids.size(); ++i)
                    if (mids[i] == ic.sdpMid) ic.sdpMLineIndex = static_cast<int>(i);
                mine.push_back(std::move(ic));
            }
        }
        XC_LOGI("sending