           if (q1 != std::string::npos) q1 = text.find('"', q1 + 1);
                auto q2 = q1 != std::string::npos ? text.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1)
                    completeTransaction(text.substr(q1 + 1, q2 - q1 - 1), json::Value::object());
            }
            return;
        }
        std::string type = (*j)["type"].str();
        if (type == "HandshakeAck") {
            onHandshakeAck();
            return;
        }
        if (type == "SenderCancel") {
            XC_LOGI("message channel: the server withdrew %s", (*j)["id"].str().c_str());
            if (cb.textInputCancelled) cb.textInputCancelled((*j)["id"].str());
            return;
        }
        // Il server chiude le nostre transazioni: niente da fare.
        if (type == "TransactionComplete" || type == "ReceiverCancel") {
            XC_LOGI("message channel: tipo %s id %s", type.c_str(), (*j)["id"].str().c_str());
            return;
        }
        if (type != "Message" && type != "TransactionStart") {
            XC_LOGI("message channel: tipo %s", type.c_str());
            return;
        }
        std::string target = (*j)["target"].str();
        std::string id = (*j)["id"].str();
        XC_LOGI("message %s %s", type.c_str(), target.c_str());

        // Tastiera: aspetta la risposta dell'utente. Tutte le altre
        // Message/TransactionStart con id vanno chiuse subito, altrimenti
        // dopo ~8s il server spegne l'RTP.
        if (target == "/streaming/systemUi/messages/ShowVirtualKeyboard" && cb.textInput) {
            auto content = json::parse((*j)["content"].str());
            TextInputRequest req;
            req.id = id;
            if (content) {
                req.title = (*content)["TitleText"].str();
                req.description = (*content)["DescriptionText"].str();
                req.defaultText = (*content)["DefaultText"].str();
                r