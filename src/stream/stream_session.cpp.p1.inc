 return;
        try {
            ch->send(reinterpret_cast<const rtc::byte*>(data.data()), data.size());
        } catch (const std::exception& e) {
            XC_LOGW("send on %s: %s", ch->label().c_str(), e.what());
        }
    }

    void sendText(const std::shared_ptr<rtc::DataChannel>& ch, const std::string& text) {
        std::lock_guard<std::mutex> lock(dcSendMu);
        sendTextUnlocked(ch, text);
    }

    void sendBinary(const std::shared_ptr<rtc::DataChannel>& ch, const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(dcSendMu);
        sendBinaryUnlocked(ch, data);
    }

    void sendInputBinary(const std::vector<uint8_t>& data) { sendBinary(input, data); }

    // Dai callback di libdatachannel: non chiamare DataChannel::send. Accoda.
    void queueText(const std::shared_ptr<rtc::DataChannel>& ch, std::string text) {
        if (!ch) return;
        std::lock_guard<std::mutex> lock(outMu);
        if (outText.size() > 64) outText.erase(outText.begin(), outText.begin() + 32);
        outText.emplace_back(ch, std::move(text));
    }

    void flushOutbound() {
        std::vector<std::pair<std::shared_ptr<rtc::DataChannel>, std::string>> batch;
        {
            std::lock_guard<std::mutex> lock(outMu);
            batch.swap(outText);
        }
        if (keyframeJsonWanted.exchange(false)) {
            json::Value v = json::Value::object();
            v.set("message", "videoKeyframeRequested");
            v.set("ifrRequested", true);
            batch.emplace_back(control, v.dump());
        }
        if (batch.empty()) return;
        std::lock_guard<std::mutex> lock(dcSendMu);
        for (auto& [ch, text] : batch) sendTextUnlocked(ch, text);
    }

    static std::string asText(const rtc::message_variant& m) {
        if (auto s = std::get_if<std::string>(&m)) return *s;
        const auto& b = std::get<rtc::binary>(m);
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }

    // --- message channel -----------------------------------------------------

    std::string messageEnvelope(const std::string& target, const json::Value& content) {
        json::Value v = json::Value::object();
        v.set("type", "Message");
        v.set("content", content.dump());
        v.set("id", uuid4());
        v.set("target", target);
        v.set("cv", "");
        return v.dump();
    }

    void completeTransaction(const std::string& id, const json::Value& content) {
        json::Value v = json::Value::object();
        v.set("type", "TransactionComplete");
        v.set("content", content.dump());
        v.set("id", id);
        v.set("cv", "");
        // Come il client web: risposta immediata sul canale message. Accodare
        // a pumpFast rischia il timeout ~8 s se il main è occupato (decode/blit).
        // Il deadlock SCTP era sugli invii input dal path RTP, non qui.
        sendText(message, v.dump());
    }

    void cancelTransaction(const std::string& id) {
        json::Value v = json::Value::object();
        v.set("type", "ReceiverCancel");
        v.set("id", id);
        v.set("cv", "");
        sendText(message, v.dump());
    }

    void sendResolution(const std::string& alias) {
        // The HQ tiers and 1440 need Game Pass Ultimate in a market where
        // they're enabled; elsewhere the service ignores them and keeps what
        // it had (1080 at the start). The plain tier first, then: measured,
        // "720" switches in a few seconds, "720HQ" is ignored in Brazil.
        // 1440 alone, as the xbox.com client sends it (never 1080 before it).
        std::string base = alias;
        if (base.size() > 2 && base.compare(base.size() - 2, 2, "HQ") == 0) base.resize(base.size() - 2);
        // The user's own Xbox offers every tier on any plan: just the one asked for.
        if (gssv.isHome()) base = alias;
        auto send = [this](const std::string& a) {
            json::Value res = json::Value::object();
        