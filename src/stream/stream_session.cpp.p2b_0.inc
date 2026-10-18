eq.inputScope = static_cast<int>((*content)["InputScope"].asInt());
                req.maxLength = static_cast<int>((*content)["MaxLength"].asInt());
            }
            XC_LOGI("the game asks for text (scope %d, up to %d characters)", req.inputScope, req.maxLength);
            cb.textInput(req);
            return;
        }

        if (target == "/streaming/properties/titleinfo") {
            auto info = json::parse((*j)["content"].str());
            if (info && cb.titleFocus) cb.titleFocus((*info)["focused"].asBool());
            if (!id.empty()) {
                XC_LOGI("message: titleinfo chiusa");
                completeTransaction(id, json::Value::object());
            }
            return;
        }

        if (target.find("serverInitiatedDisconnect") != std::string::npos) {
            std::string body = (*j)["content"].str();
            XC_LOGW("server disconnect: %s", body.substr(0, 300).c_str());
            // Risposta immediata (thread message): senza TransactionComplete il
            // server chiude DTLS prima che pumpFast flushi la coda.
            {
                json::Value v = json::Value::object();
                v.set("type", "TransactionComplete");
                v.set("content", json::Value("").dump());
                v.set("id", id);
                v.set("cv", "");
                sendText(message, v.dump());
            }
            auto content = json::parse(body);
            std::string reason = content ? (*content)["reason"].str() : std::string();
            if (reason.empty() && content) reason = (*content)["Reason"].str();
            XC_LOGW("server disconnect reason: %s", reason.c_str());
            if (reason == "WarningForBeingIdle") {
                // Non chiudere: il client deve mandare input reale. Segnala.
                if (cb.idleWarning) cb.idleWarning(static_cast<int>((*content)["secondsUntilKick"].asInt(120)));
            } else if (reason == "KickForClosedGame") {
                char hr[1