   hs.set("version", "messageV1");
            hs.set("id", kHandshakeId);
            hs.set("cv", "0");
            sendText(message, hs.dump());
        });
        message->onMessage([this](rtc::message_variant m) { onMessageChannel(m); });
        control->onMessage([](rtc::message_variant m) { XC_LOGD("control: %s", asText(m).substr(0, 300).c_str()); });
        input->onMessage([this](rtc::message_variant m) {
            auto b = std::get_if<rtc::binary>(&m);
            if (!b) return;
            auto* d = reinterpret_cast<const uint8_t*>(b->data());
            Vibration v;
            uint32_t w, h;
            if (parseVibration(d, b->size(), v)) {
                if (cb.vibration) cb.vibration(v);
            } else if (parseServerMetadata(d, b->size(), w, h)) {
                XC_LOGI("server video size %ux%u", w, h);
            } else if (b->size() >= 2 && ++otherInputReports <= 5) {
                XC_LOGI("input channel: report type %u, %zu bytes", d[0], b->size());
            }
        });

        // Audio: sendrecv like the browser (the send side carries chat).
        rtc::Description::Audio a("0", rtc::Description::Direction::SendRecv);
        a.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1");
        a.addSSRC(1, "audio");
        audio = pc->addTrack(a);

        auto audioDepacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
        audioDepacketizer->addToChain(std::make_shared<RtcpReporter>(48000, 0));

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            1, "audio", 111, rtc::OpusRtpPacketizer::DefaultClockRate);
        auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
        audioDepacketizer->addToChain(audioPacketizer);

        audio->setMediaHandler(audioDepacketizer);
        audio->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            static int once = 0;
            if (!once++) XC_LOGI("audio: primo opus %zu", data.size());
            if