 lock(mutex);
                    gatheringDone = true;
                }
                cv.notify_all();
            }
        });
        pc->onStateChange([this](rtc::PeerConnection::State s) {
            XC_LOGI("peer connection state: %d", static_cast<int>(s));
            if (s == rtc::PeerConnection::State::Failed) fail("WebRTC connection failed");
            // Disconnected è transitorio: chiudere qui fa ricomparire la
            // schermata del controller. Si resta in sessione finché non è Failed.
            if (s == rtc::PeerConnection::State::Disconnected)
                XC_LOGW("webrtc: disconnected, resto in sessione");
        });
        pc->onIceStateChange([this](rtc::PeerConnection::IceState s) {
            XC_LOGI("ice: stato %d", static_cast<int>(s));
            if (s == rtc::PeerConnection::IceState::Connected || s == rtc::PeerConnection::IceState::Completed) {
                std::lock_guard<std::mutex> lock(mutex);
                iceUp = true;
                cv.notify_all();
            }
            if (s == rtc::PeerConnection::IceState::Failed) fail("ICE failed");
        });

        // Same channels, in the same order, as the web client.
        chat = makeChannel("chat", "chatV1");
        chat->onMessage([](rtc::message_variant m) { XC_LOGI("chat channel: %s", asText(m).c_str()); });
        control = makeChannel("control", "controlV1");
        control->onOpen([this] {
            XC_LOGI("data channel control open");
            announceGamepad();
        });
        input = makeChannel("input", "1.0");
        input->onOpen([this] {
            XC_LOGI("data channel input open");
            GamepadFrame f;
            f.index = 0;
            sendPad(f);
        });
        message = makeChannel("message", "messageV1");

        message->onOpen([this] {
            XC_LOGI("data channel message open: sending handshake");
            json::Value hs = json::Value::object();
            hs.set("type", "Handshake");
         