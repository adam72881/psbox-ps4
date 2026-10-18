;
        flushOutbound();
        sendPadIfDue();
        if (videoReporter && videoReporter->secondsQuiet() >= 2) {
            uint64_t now = platform::nowMs();
            if (now - lastQuietKf >= 2000) {
                lastQuietKf = now;
                requestKeyframe();
                XC_LOGI("rtcp: video fermo da %d s, keyframe", videoReporter->secondsQuiet());
            }
        }
    }

    void tick() {
        pumpFast();
        if (g_streamStop.load()) return;
        if (control && control->isOpen() && (!padAnnounceSent || padsStale.exchange(false))) announceGamepad();
        if (!open) return;
        uint64_t now = platform::nowMs();
        if (now - lastKeepaliveMs >= 10000) {
            lastKeepaliveMs = now;
            std::string err;
            if (!gssv.keepalive(err)) XC_LOGW("%s", err.c_str());
            else {
                static int once = 0;
                if (!once++) XC_LOGI("keepalive ok");
            }
        }
        if (opt.keyframeIntervalSec > 0 && now - lastKeyframeMs >= static_cast<uint64_t>(opt.keyframeIntervalSec) * 1000u) {
            lastKeyframeMs = now;
            requestKeyframe();
        }
    }

    // Before the session goes: no callback of libdatachannel's (they hold
    // `this`) may run any more. Each reset waits for a call in progress.
    void detach() {
        {
            std::lock_guard<std::mutex> lock(guard->mutex);
            guard->self = nullptr;
        }
        try {
            for (const auto& ch : {chat, control, input, message})
                if (ch) ch->resetCallbacks();
            for (const auto& track : {audio, video})
                if (track) {
                    track->onFrame(nullptr);
                    track->resetCallbacks();
                }
            if (pc) pc->resetCallbacks();
        } catch (const std::exception& e) {
            XC_LOGW("detaching the session: %s", e.what());
        }
    }

    void close() {
        open = false;
        if (p