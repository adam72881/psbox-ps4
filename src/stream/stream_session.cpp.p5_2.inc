rue;
        }
        err = localErr;
        return false;
    }

    // Come 01.35: ack+pad nello stesso datagramma (inputReport). Gli ack
    // restano in coda finché pumpFast/sendPad (mai send dal thread video).
    void sendPad(const GamepadFrame& frame) {
        if (!input || !input->isOpen() || g_streamStop.load()) return;
        std::vector<FrameMetadata> frames;
        uint32_t seq;
        {
            std::lock_guard<std::mutex> lock(padMu);
            if (frame.index < 4) {
                lastPad[frame.index] = frame;
                lastPadMs[frame.index] = platform::nowMs();
            }
            if (pendingFrames.size() > 30)
                pendingFrames.erase(pendingFrames.begin(), pendingFrames.end() - 30);
            frames.swap(pendingFrames);
            seq = inputSequence++;
        }
        static int once = 0;
        if (!once++) XC_LOGI("input: primo pad");
        if (!frames.empty()) {
            uint64_t prev = acked.fetch_add(frames.size());
            if (prev == 0) XC_LOGI("input: ack frame");
        }
        sendInputBinary(inputReport(seq, nowMs(), &frame, frames));
    }

    void requestKeyframe() {
        json::Value v = json::Value::object();
        v.set("message", "videoKeyframeRequested");
        v.set("ifrRequested", true);
        sendText(control, v.dump());
    }

    uint64_t lastQuietKf = 0;

    void sendPadIfDue() {
        if (!input || !input->isOpen() || g_streamStop.load()) return;
        GamepadFrame f;
        f.index = 0;
        uint64_t now = platform::nowMs();
        bool due = false;
        {
            std::lock_guard<std::mutex> lock(padMu);
            due = lastPadMs[0] == 0 || now - lastPadMs[0] >= kPadRepeatMs || !pendingFrames.empty();
            if (due) f = lastPad[0];
        }
        if (due) {
            f.index = 0;
            sendPad(f);
        }
    }

    void pumpFast() {
        if (g_streamStop.load()) return;
        if (videoReporter) videoReporter->pump()