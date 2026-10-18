 g->self->keyframeJsonWanted = true;
            },
            videoStats);
        videoReceiver->setSimulatedLoss(opt.simulatedVideoLoss);
        videoReporter = std::make_shared<RtcpReporter>(90000, opt.maxBitrate);
        videoReceiver->addToChain(videoReporter);
        video->setMediaHandler(videoReceiver);
        XC_LOGI("webrtc: data channel e tracce audio/video pronte");
        video->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            static int once = 0;
            if (!once++) XC_LOGI("video: primo frame %zu", data.size());
            if (resolutionPending.exchange(false)) sendResolution(opt.resolutionAlias);
            uint64_t now = platform::nowMs();
            uint64_t last = lastVideoMs.exchange(now);
            if (last && now - last >= 1500) {
                XC_LOGI("picture back after %llu ms: announcing the controllers again",
                        static_cast<unsigned long long>(now - last));
                padsStale = true;
            }
            if (cb.video) cb.video(reinterpret_cast<const uint8_t*>(data.data()), data.size(), info.timestamp);
        });
    }

    bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (g_streamStop.load()) {
                failed = true;
                if (failReason.empty()) failReason = "chiusura";
                return false;
            }
            if (failed) return false;
            if (pred()) return true;
            const auto now = std::chrono::steady_clock::now();
            if (now >= end) return false;
            auto slice = std::chrono::milliseconds(200);
            if (end - now < slice) slice = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
            cv.wait_for(lock, slice, [&] { return failed || pred() || g_streamStop.load(); });
        