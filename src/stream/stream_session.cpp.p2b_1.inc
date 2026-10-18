6];
                std::snprintf(hr, sizeof hr, "0x%08X", static_cast<unsigned>((*content)["hr"].asInt()));
                fail(std::string("the game closed on the server (") + hr + ")", false);
            } else {
                fail(reason.empty() ? "the server ended the session" : "the server ended the session (" + reason + ")",
                     false);
            }
            return;
        }

        if (target == "/streaming/systemUi/messages/ShowMessageDialog") {
            auto content = json::parse((*j)["content"].str());
            if (content)
                XC_LOGW("server dialog: %s / %s", (*content)["TitleText"].str().c_str(),
                        (*content)["ContentText"].str().c_str());
            json::Value result = json::Value::object();
            result.set("Result", 0);
            completeTransaction(id, result);
            return;
        }

        // ShowGameTimeNotification, touchcontrols/showtitledefault, ecc.
        if (!id.empty()) {
            XC_LOGI("message: transazione %s chiusa", target.c_str());
            completeTransaction(id, json::Value::object());
        }
    }

    // --- set-up ---------------------------------------------------------------

    std::shared_ptr<rtc::DataChannel> makeChannel(const char* label, const char* protocol) {
        rtc::DataChannelInit init;
        init.protocol = protocol;
        auto ch = pc->createDataChannel(label, init);
        ch->onOpen([label] { XC_LOGI("data channel %s open", label); });
        ch->onClosed([this, label] {
            XC_LOGI("data channel %s closed", label);
            // La chat vocale si chiude da sola se non c'è il microfono: non è
            // la sessione di gioco. Gli altri canali sì.
            // chat e message sono canali di interfaccia. Chiuderli non è la
            // sessione di gioco: control e input restano, e si continua a
            // mandare pad, ack e RTCP. È la chiusura di message che faceva
            // comparire 