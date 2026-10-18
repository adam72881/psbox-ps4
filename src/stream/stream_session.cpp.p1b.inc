    res.set("message", "userRequestedResolutionUpdate");
            res.set("resolutionAlias", a);
            sendText(control, res.dump());
            XC_LOGI("requested stream resolution %s", a.c_str());
        };
        send(base);
        if (alias != base) send(alias);
    }

    void sendClientConfig() {
        // Stesso ordine e stessi target di xbox-xcloud-player message.ts
        // sendConfig(): configuration, clientappinstallid, orientation,
        // touch, clientdevicecapabilities, dimensions. Niente TransactionStart
        // su /streaming/title/constrain: il client web non lo manda.
        json::Value ver = json::Value::array();
        ver.push(0);
        ver.push(2);
        ver.push(0);
        json::Value ui = json::Value::object();
        ui.set("version", ver);
        // Vuoto come xbox-xcloud-player (message.ts). Dichiarare [10,19,…]
        // fa arrivare ShowGameTimeNotification e simili: se una
        // TransactionComplete non parte entro ~8 s il server spegne tutto
        // l'RTP (video+audio+ack) con RTT ancora buono — sintomo 01.27.
        json::Value systemUis = json::Value::array();
        if (cb.textInput) systemUis.push(kShowVirtualKeyboard);
        ui.set("systemUis", systemUis);
        sendText(message, messageEnvelope("/streaming/systemUi/configuration", ui));

        json::Value install = json::Value::object();
        install.set("clientAppInstallId", kClientAppInstallId);
        sendText(message, messageEnvelope("/streaming/properties/clientappinstallidchanged", install));

        json::Value orientation = json::Value::object();
        orientation.set("orientation", 0);
        sendText(message, messageEnvelope("/streaming/characteristics/orientationchanged", orientation));

        json::Value touch = json::Value::object();
        touch.set("touchInputEnabled", false);
        sendText(message, messageEnvelope("/streaming/characteristics/touchinputenabledchanged", touch));

        sendText(message, messageEnvelope("/streaming/characteristics/clientdevicecapabilities", json::Value::object()));

        // Come il client web: horizontal/vertical sono i pixel dell'elemento
        // video, non i millimetri del pannello.
        int w = opt.resolutionAlias == "1440" ? 2560 : opt.resolutionAlias == "720" ? 1280 : 1920;
        int h = opt.resolutionAlias == "1440" ? 1440 : opt.resolutionAlias == "720" ? 720 : 1080;
        json::Value dims = json::Value::object();
        dims.set("horizontal", w);
        dims.set("vertical", h);
        dims.set("preferredWidth", w);
        dims.set("preferredHeight", h);
        dims.set("safeAreaLeft", 0);
        dims.set("safeAreaTop", 0);
        dims.set("safeAreaRight", w);
        dims.set("safeAreaBottom", h);
        dims.set("supportsCustomResolution", true);
        sendText(message, messageEnvelope("/streaming/characteristics/dimensionschanged", dims));
        XC_LOGI("message: config client inviata %dx%d systemUis %s", w, h,
                cb.textInput ? "tastiera" : "vuoto");
    }

    void sendGamepadChanged(int index, bool added) {
        if (index >= 0 && index < 32) {
            if (added) padsAttached |= 1u << index;
            else padsAttached &= ~(1u << index);
        }
        json::Value v = json::Value::object();
        v.set("message", "gamepadChanged");
        v.set("gamepadIndex", index);
        v.set("wasAdded", added);
        sendText(control, v.dump());
    }

    void announceGamepad() {
        if (!control || !control->isOpen()) return;
        if (!padKindLogged) {
            padKindLogged = true;
            // Il wire protocol xCloud è la bitmask Xbox; sul pad fisico è DS4.
            XC_LOGI("input: pad DS4 (protocollo Xbox bitmask)");
        }
        sendGamepadChanged(0, true);
        sendGamepadChanged(0, false);
        sendGamepadChanged(0, true);
        padAnnounceSent = true;
        XC_LOGI("input: pad connesso");
    }

    void onHandshakeAck() {
        XC_LOGI("m