essage channel handshake complete");
        json::Value auth = json::Value::object();
        auth.set("message", "authorizationRequest");
        auth.set("accessKey", kControlAccessKey);
        sendText(control, auth.dump());
        // Il protocollo input 1.0 (xbox-xcloud-player e il PSBox PS5 fino a
        // main) non ha un tipo di controller: il frame è la bitmask Xbox e
        // gamepadChanged porta solo indice e wasAdded. Niente vendor 054c.
        // L'annuncio all'apertura di control può arrivare prima
        // dell'authorizationRequest: il server lo ignora e Fortnite mostra
        // il controller. Si riannuncia sempre dopo l'ack dell'handshake.
        announceGamepad();
        // 1440 once the picture is running (the first video frame), as the
        // xbox.com client sends it; the other tiers right away.
        if (opt.resolutionAlias == "1440" && !gssv.isHome())
            resolutionPending = true;
        else if (!opt.resolutionAlias.empty())
            sendResolution(opt.resolutionAlias);


        // One touch point, as the web client announces (touch is then turned
        // on only while a touch screen is up: setTouchEnabled()).
        sendBinary(input, clientMetadataReport(0, nowMs(), static_cast<uint8_t>(std::getenv("XC_TOUCH_POINTS") ? std::atoi(std::getenv("XC_TOUCH_POINTS")) : 1)));
        sendClientConfig();
        {
            std::lock_guard<std::mutex> lock(mutex);
            handshakeDone = true;
        }
        open = true;
        cv.notify_all();
    }

    void onMessageChannel(const rtc::message_variant& m) {
        std::string text = asText(m);
        auto j = json::parse(text);
        if (!j) {
            XC_LOGW("message channel: unparsable %s", text.substr(0, 200).c_str());
            // Meglio chiudere una transazione a caso che lasciarla aperta 8 s.
            auto idPos = text.find("\"id\"");
            if (idPos != std::string::npos) {
                auto q1 = text.find('"', idPos + 4);
     