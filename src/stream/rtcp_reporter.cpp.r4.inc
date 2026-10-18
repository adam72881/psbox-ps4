           chunks.push_back(static_cast<uint16_t>(run & 0x1FFF));  // T=0 S=0
            i += run;
            continue;
        }
        // Un pacchetto alla volta per scegliere small/large correttamente.
        uint64_t us = arr[static_cast<size_t>(i)];
        int64_t dUs = static_cast<int64_t>(us) - static_cast<int64_t>(prevUs);
        prevUs = us;
        int64_t ticks = dUs / 250;
        if (ticks >= -128 && ticks <= 127) {
            chunks.push_back(static_cast<uint16_t>((1u << 13) | 1));  // S=1 run=1
            deltas.push_back(static_cast<uint8_t>(static_cast<int8_t>(ticks)));
        } else {
            if (ticks > 0x7FFF) ticks = 0x7FFF;
            if (ticks < -0x8000) ticks = -0x8000;
            chunks.push_back(static_cast<uint16_t>((2u << 13) | 1));  // S=2 large
            auto v = static_cast<uint16_t>(static_cast<int16_t>(ticks));
            deltas.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
            deltas.push_back(static_cast<uint8_t>(v & 0xff));
        }
        ++i;
    }

    std::vector<uint8_t> b;
    b.reserve(32 + chunks.size() * 2 + deltas.size() + 4);
    b.push_back(0x8F);
    b.push_back(205);
    put16(b, 0);
    put32(b, kReceiverSsrc);
    put32(b, mediaSsrc_);
    put16(b, base);
    put16(b, static_cast<uint16_t>(statusCount));
    b.push_back(static_cast<uint8_t>((refMs64 >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((refMs64 >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>(refMs64 & 0xff));
    b.push_back(twccFbCount_);
    for (uint16_t c : chunks) put16(b, c);
    b.insert(b.end(), deltas.begin(), deltas.end());
    while (b.size() % 4) b.push_back(0);
    size_t words = b.size() / 4 - 1;
    b[2] = static_cast<uint8_t>((words >> 8) & 0xff);
    b[3] = static_cast<uint8_t>(words & 0xff);

    auto* p = reinterpret_cast<const std::byte*>(b.data());
    try {
        send(rtc::make_message(p, p + b.size(), rtc::Message::Control));
    } catch (const std::exception& e) {
        XC_LOGW("rtcp: