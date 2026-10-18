c) {
            try {
                pc->close();
            } catch (const std::exception& e) {
                XC_LOGW("closing peer connection: %s", e.what());
            }
        }
    }
};

StreamSession::StreamSession(xcloud::GssvClient& gssv, StreamCallbacks callbacks, StreamOptions options)
    : impl_(std::make_unique<Impl>(gssv, std::move(callbacks), options)) {}

StreamSession::~StreamSession() {
    impl_->closedNotified = true;  // no callback while tearing down
    impl_->close();
    impl_->detach();
}

bool StreamSession::start(std::string& err) {
    // libdatachannel reports set-up failures (e.g. no usable socket) by throwing.
    try {
        return impl_->start(err);
    } catch (const std::exception& e) {
        err = std::string("WebRTC: ") + e.what();
    } catch (...) {
        err = "WebRTC: unknown error";
    }
    impl_->close();
    return false;
}
bool StreamSession::isOpen() const { return impl_->open; }

void StreamSession::sendGamepad(const GamepadFrame& frame) {
    Impl& d = *impl_;
    if (d.padsStale.exchange(false)) d.announceGamepad();
    if (!d.input || !d.input->isOpen()) return;
    {
        std::lock_guard<std::mutex> lock(d.padMu);
        uint64_t now = platform::nowMs();
        if (frame.index < 4 && d.lastPadMs[frame.index] && frame == d.lastPad[frame.index] &&
            now - d.lastPadMs[frame.index] < Impl::kPadRepeatMs && d.pendingFrames.empty())
            return;
    }
    d.sendPad(frame);
}

void StreamSession::sendAudio(const uint8_t* opus, size_t size, uint32_t timestamp) {
    if (!impl_ || !impl_->open || !impl_->audio || !opus || size == 0) return;
    try {
        rtc::FrameInfo info(timestamp);
        impl_->audio->sendFrame(reinterpret_cast<const rtc::byte*>(opus), size, info);
    } catch (const std::exception&) {
    }
}

void StreamSession::setTouchEnabled(bool on) {
    if (!impl_->open) return;
    json::Value touch = json::Value::object();
    touch.set("touchInputEnabled", on);
    impl_->sendText(impl_->message, impl_->messageEnvelope("/streaming/characteristics/touchinputenabledchanged", touch));
}

void StreamSession::setGamepadConnected(int index, bool connected) {
    if (!impl_->open) return;
    XC_LOGI("gamepad %d %s", index, connected ? "attached" : "detached");
    impl_->sendGamepadChanged(index, connected);
}

void StreamSession::requestKeyframe() {
    try {
        impl_->requestKeyframe();
    } catch (const std::exception& e) {
        XC_LOGW("keyframe request: %s", e.what());
    }
}
void StreamSession::reportFrame(const FrameMetadata& frame) {
    // Accoda solo: il send sul DataChannel "input" avviene in sendPad /
    // pumpFast (thread main/worker). Un DataChannel::send dal thread video
    // (o dal drain del decoder) mentre juice/SCTP processano un altro
    // messaggio crea deadlock usrsctp (coda DTLS piena, pad appeso su
    // inputSendMu) — riprodotto sull'harness Linux.
    if (!impl_->open || g_streamStop.load()) return;
    std::lock_guard<std::mutex> lock(impl_->padMu);
    impl_->pendingFrames.push_back(frame);
    if (impl_->pendingFrames.size() > 60)
        impl_->pendingFrames.erase(impl_->pendingFrames.begin(), impl_->pendingFrames.end() - 60);
}

void StreamSession::retire() {
    impl_->closedNotified = true;
    impl_->detach();
    impl_->close();
}

void StreamSession::abandon() {
    impl_->closedNotified = true;
    impl_->open = false;
    // Solo il guard: le callback diventano no-op. Niente resetCallbacks —
    // quella attende la callback in corso (RTP/juice) e teneva dieNow
    // appeso dopo uno stream → il sistema revocava Videodec2/Audio/VideoOut
    // e la PS4 mostrava CE-34878-0. Niente pc->close().
    if (impl_->guard) {
        std::lock_guard<std::mutex> lock(impl_->guard->mutex);
        impl_->guard->self = nullptr;
    }
}

void StreamSession::armNeutralPad() {
    GamepadFrame f;
    f.index = 0;
    std::lock_guard<std::mutex> lock(impl_->padMu);
    impl_->lastPad[0] = f;
    // lastPadMs a 0: il prossimo pumpFast manda subito.
    impl_->lastPadMs[0] = 0;
}

uint64_t StreamSession::framesAcked() const { return impl_->acked.load(); }

double StreamSession::clockMs() const { return stream::clockMs(); }

void StreamSession::simulateDrop() {
    XC_LOGW("test: dropping the connection");
    if (impl_->pc) impl_->pc->close();
}

double clockMs() {
    static const auto epoch = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - epoch).count();
}

void StreamSession::completeTextInput(const std::string& id, const std::string& text) {
    json::Value v = json::Value::object();
    v.set("Text", text);
    impl_->completeTransaction(id, v);
}

void StreamSession::cancelTextInput(const std::string& id) { impl_->cancelTransaction(id); }

void StreamSession::requestResolution(const std::string& alias) { impl_->sendResolution(alias); }

void StreamSession::pumpFast() { impl_->pumpFast(); }
void StreamSession::tick() { impl_->tick(); }
void StreamSession::close() { impl_->close(); }
int StreamSession::rttMs() const {
    try {
        if (impl_->pc)
            if (auto rtt = impl_->pc->rtt()) return static_cast<int>(rtt->count());
    } catch (const std::exception&) {
    }
    return -1;
}

const VideoReceiveStats& StreamSession::videoStats() const {
    if (impl_->videoReporter) {
        impl_->videoStats.receiveRate = impl_->videoReporter->receiveRate();
        impl_->videoStats.estimate = impl_->videoReporter->estimate();
        impl_->videoStats.rtcpReports = impl_->videoReporter->reportsSent();
        impl_->videoStats.twccSent = impl_->videoReporter->twccSent();
    }
    return impl_->videoStats;
}

}  // namespace xc::stream
