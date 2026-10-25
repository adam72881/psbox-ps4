// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "stream/stream_session.h"

#include "stream/connect_plan.h"

#include "stream/h264_receiver.h"
#include "stream/rtcp_reporter.h"

#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"

#if defined(XCLOUD_PS4)
#include "app/stream_trace.h"
#include "platform/ps4/entropy_ps4.h"
#endif

#include <rtc/rtc.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace xc::stream {

namespace {

// Fixed values the xbox.com web client sends.
constexpr const char* kControlAccessKey = "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E";
constexpr const char* kHandshakeId = "be0bfc6d-1e83-4c8a-90ed-fa8601c5a179";
constexpr const char* kClientAppInstallId = "c97d7ee0-73b2-4239-bf1d-9d805a338429";
// SystemUiType che il client web xCloud dichiara in systemUis
// (xbox-xcloud-player message.ts). Senza questi il server manda comunque
// ShowGameTimeNotification e simili; con questi ci aspettiamo le Message
// e dobbiamo rispondere con TransactionComplete.
constexpr int kShowVirtualKeyboard = 10;
// Altri UI xCloud (19/31/27/32/-41): non dichiararli — vedi sendClientConfig.

std::string uuid4() {
    uint8_t b[16];
    platform::randomBytes(b, sizeof b);
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);
    char s[37];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

std::vector<std::string> midsInOrder(const std::string& sdp) {
    std::vector<std::string> mids;
    size_t pos = 0;
    while ((pos = sdp.find("a=mid:", pos)) != std::string::npos) {
        pos += 6;
        size_t end = sdp.find_first_of("\r\n", pos);
        mids.push_back(sdp.substr(pos, end - pos));
    }
    return mids;
}

// Porte m= video/audio/application dall'SDP (AJ-GFN: ICE ok su una porta,
// DTLS solo su un'altra).
std::vector<int> mediaPortsFromSdp(const std::string& sdp) {
    std::vector<int> ports;
    size_t pos = 0;
    while (pos < sdp.size()) {
        size_t line = (pos == 0 && sdp.compare(0, 2, "m=") == 0) ? 0 : sdp.find("\nm=", pos);
        if (line == std::string::npos) break;
        if (line > 0) ++line;  // skip '\n'
        size_t p = line + 2;
        while (p < sdp.size() && sdp[p] != ' ') ++p;
        while (p < sdp.size() && sdp[p] == ' ') ++p;
        int port = 0;
        while (p < sdp.size() && sdp[p] >= '0' && sdp[p] <= '9') {
            port = port * 10 + (sdp[p] - '0');
            ++p;
        }
        if (port > 0) ports.push_back(port);
        pos = line + 2;
    }
    return ports;
}

void initRtcLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
        rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel level, std::string message) {
            if (level <= rtc::LogLevel::Error)
                XC_LOGE("rtc: %s", message.c_str());
            else if (level == rtc::LogLevel::Warning)
                XC_LOGW("rtc: %s", message.c_str());
            else
                XC_LOGI("rtc: %s", message.c_str());
        });
    });
}

}  // namespace

bool decodeTeredo(const std::string& ipv6, std::string& ipv4, int& port) {
    in6_addr a{};
    if (::inet_pton(AF_INET6, ipv6.c_str(), &a) != 1) return false;
    const uint8_t* b = a.s6_addr;
    if (!(b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00)) return false;
    // Bytes 10-11: obfuscated (inverted) port; 12-15: inverted client IPv4.
    port = static_cast<uint16_t>(~((b[10] << 8) | b[11]));
    char s[16];
    std::snprintf(s, sizeof s, "%u.%u.%u.%u", b[12] ^ 0xff, b[13] ^ 0xff, b[14] ^ 0xff, b[15] ^ 0xff);
    ipv4 = s;
    return true;
}

std::atomic<bool> g_streamStop{false};
std::atomic<bool> g_exitStop{false};

static bool stopRequested() { return g_streamStop.load() || g_exitStop.load(); }

void requestStop() {
    StopFlags f{g_streamStop.load() ? 1 : 0, g_exitStop.load() ? 1 : 0};
    onRequestStop(&f);
    g_streamStop.store(f.streamStop != 0);
    g_exitStop.store(f.exitStop != 0);
}

void armNewSession() {
    StopFlags f{g_streamStop.load() ? 1 : 0, g_exitStop.load() ? 1 : 0};
    if (!onArmNewSession(&f)) {
        g_streamStop.store(true);
        XC_LOGI("stream: nuova sessione rifiutata, chiusura in corso");
        return;
    }
    g_streamStop.store(false);
    XC_LOGI("stream: nuova sessione, stop azzerato");
}

void logSdpLine(const char* which, const std::string& sdp, const char* key) {
    auto p = sdp.find(key);
    if (p == std::string::npos) {
        XC_LOGI("sdp %s %s assente", which, key);
        return;
    }
    auto e = sdp.find_first_of("\r\n", p);
    XC_LOGI("sdp %s %s", which, sdp.substr(p, e == std::string::npos ? 120 : e - p).c_str());
}

struct StreamSession::Impl {
    xcloud::GssvClient& gssv;
    StreamCallbacks cb;
    StreamOptions opt;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> chat, control, input, message;
    std::shared_ptr<rtc::Track> audio, video;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<rtc::Candidate> localCandidates;
    bool gatheringDone = false;
    bool handshakeDone = false;
    bool iceUp = false;
    bool failed = false;
    std::string failReason;

    std::atomic<bool> open{false};
    std::atomic<bool> resolutionPending{false};  // 1440: asked at the first video frame
    std::atomic<bool> closedNotified{false};
    std::atomic<uint32_t> inputSequence{1};
    uint64_t lastKeepaliveMs = 0;
    uint64_t lastKeyframeMs = 0;
    VideoReceiveStats videoStats;
    std::shared_ptr<RtcpReporter> videoReporter;
    // The video receiver (a media handler, which libdatachannel may still be
    // running after the session is gone) reaches the session through this.
    struct Guard {
        std::mutex mutex;
        Impl* self = nullptr;
    };
    std::shared_ptr<Guard> guard = std::make_shared<Guard>();
    std::atomic<int> otherInputReports{0};
    // Controllers: which slots are attached; what each sent last and when
    // (sendGamepad's thread). A pad that didn't change is sent again only
    // every kPadRepeatMs, not at the input loop's rate.
    static constexpr uint64_t kPadRepeatMs = 50;
    std::atomic<uint32_t> padsAttached{0};
    GamepadFrame lastPad[4];
    uint64_t lastPadMs[4] = {};
    bool padKindLogged = false;
    bool padAnnounceSent = false;
    std::mutex padMu;
    // Un solo lock per TUTTI i DataChannel (input/control/message): condividono
    // lo stesso SCTP. Send concorrenti da thread diversi (RTP, message, main)
    // riempivano la coda DTLS e bloccavano juice/usrsctp (harness Linux).
    std::mutex dcSendMu;
    std::vector<FrameMetadata> pendingFrames;
    // Testi da spedire fuori dai callback di libdatachannel (flush in pumpFast).
    std::mutex outMu;
    std::vector<std::pair<std::shared_ptr<rtc::DataChannel>, std::string>> outText;
    std::atomic<bool> keyframeJsonWanted{false};
    std::atomic<uint64_t> acked{0};
    // After the picture stopped for a while (a lag spike), the server may
    // keep receiving the pad and ignore it: the pads are announced again.
    std::atomic<uint64_t> lastVideoMs{0};
    std::atomic<bool> padsStale{false};

    Impl(xcloud::GssvClient& g, StreamCallbacks c, StreamOptions o) : gssv(g), cb(std::move(c)), opt(o) {}

    double nowMs() const { return stream::clockMs(); }

    void fail(const std::string& reason, bool recoverable = true) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!failed) {
                failed = true;
                failReason = reason;
            }
        }
        cv.notify_all();
        open = false;
        if (!closedNotified.exchange(true)) {
            XC_LOGW("stream closed: %s", reason.c_str());
            if (cb.closed) cb.closed(reason, recoverable);
        }
    }

    static void sendTextUnlocked(const std::shared_ptr<rtc::DataChannel>& ch, const std::string& text) {
        // The web client sends its JSON as binary (TextEncoder) messages.
        if (!ch || !ch->isOpen()) {
            static int drops = 0;
            if (drops < 8) {
                ++drops;
                XC_LOGW("canale send salto, %s chiuso", ch ? ch->label().c_str() : "(nullo)");
            }
            return;
        }
        try {
            ch->send(reinterpret_cast<const rtc::byte*>(text.data()), text.size());
        } catch (const std::exception& e) {
            XC_LOGW("send on %s: %s", ch->label().c_str(), e.what());
        }
    }

    static void sendBinaryUnlocked(const std::shared_ptr<rtc::DataChannel>& ch, const std::vector<uint8_t>& data) {
        if (!ch || !ch->isOpen()) return;
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
        XC_LOGI("message channel handshake complete");
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
                req.inputScope = static_cast<int>((*content)["InputScope"].asInt());
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
                char hr[16];
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
            // comparire «connessione persa».
            if (std::strcmp(label, "chat") == 0 || std::strcmp(label, "message") == 0) return;
            if (open) fail(std::string("data channel ") + label + " closed");
        });
        return ch;
    }

    void createPeer() {
        XC_LOGI("webrtc: logger");
        initRtcLogging();
        XC_LOGI("webrtc: config");
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        // Default libdatachannel = 1280. Su Orbis i TWCC grandi della 01.35
        // (count 222–314 ≈ 900–1300 B) finivano al bordo MTU; i ClientHello
        // DTLS sono ~342 B quindi il fallimento 01.36 non era MTU, ma i
        // feedback grandi sì. 1200 lascia margine su Teredo/UDP.
        config.mtu = 1200;
        /* STUN di Teams: il primo server è l'unico che libjuice interroga.
         * Senza, l'unico candidato locale è l'host di getifaddrs. */
        config.iceServers.emplace_back("worldaz.relay.teams.microsoft.com", uint16_t{3478});
        XC_LOGI("webrtc: ctor (mtu %zu)", *config.mtu);
        pc = std::make_shared<rtc::PeerConnection>(config);
        XC_LOGI("webrtc: ctor ok");

        pc->onLocalCandidate([this](rtc::Candidate c) {
            const char* kind = "altro";
            switch (c.type()) {
            case rtc::Candidate::Type::Host: kind = "host"; break;
            case rtc::Candidate::Type::ServerReflexive: kind = "srflx"; break;
            case rtc::Candidate::Type::Relayed: kind = "relay"; break;
            case rtc::Candidate::Type::PeerReflexive: kind = "prflx"; break;
            default: break;
            }
            XC_LOGI("ice: candidato locale %s", kind);
            std::lock_guard<std::mutex> lock(mutex);
            localCandidates.push_back(std::move(c));
        });
        pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
            if (s == rtc::PeerConnection::GatheringState::Complete) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
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
            if (cb.audio) cb.audio(reinterpret_cast<const uint8_t*>(data.data()), data.size(), info.timestamp);
        });

        // Video: H.264 only, Main before Constrained Baseline / Baseline.
        rtc::Description::Video v("1", rtc::Description::Direction::RecvOnly);
        // Main profile at level 5.1 (0x33): the level that allows 1440p60; with
        // 3.1 the service tops out at 1080p (it answers level 4.2).
        v.addH264Codec(102, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d0033");
        v.addH264Codec(104, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
        v.addH264Codec(106, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f");
        // Come PS5 v0.9.8 e 01.35: transport-cc nell'offerta (ext id 3).
        // La 01.36 l'ha tolto «per allinearsi al REMB-only» ma sul PS5
        // l'offerta ha ancora transport-cc: senza, su Orbis il server non
        // risponde al ClientHello DTLS (0 byte RX → timeout).
        v.addExtMap(rtc::Description::Entry::ExtMap(
            3, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"));
        for (int pt : {102, 104, 106}) {
            if (auto* m = v.rtpMap(pt)) m->addFeedback("transport-cc");
        }
        v.setBitrate(static_cast<int>(opt.maxBitrate / 1000u));
        v.addSSRC(kReceiverSsrc, "video");
        XC_LOGI("video: offro %u kbps, ssrc rtcp %08x, transport-cc", opt.maxBitrate / 1000u, kReceiverSsrc);
        video = pc->addTrack(v);
        // Our jitter buffer (NACK, whole frames only) in place of
        // H264RtpDepacketizer, which hands over frames with holes in them.
        guard->self = this;
        auto videoReceiver = std::make_shared<H264Receiver>(
            [g = guard] {
                // Solo flag: DataChannel::send dal path RTP → deadlock SCTP/DTLS.
                std::lock_guard<std::mutex> lock(g->mutex);
                if (!g->self || !g->self->open) return;
                g->self->keyframeJsonWanted = true;
            },
            videoStats);
        videoReceiver->setSimulatedLoss(opt.simulatedVideoLoss);
        videoReporter = std::make_shared<RtcpReporter>(90000, opt.maxBitrate);
        videoReceiver->addToChain(videoReporter);
        video->setMediaHandler(videoReceiver);
        XC_LOGI("webrtc: data channel e tracce audio/video pronte");
        video->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            static int once = 0;
            if (!once++) XC_LOGI("video: primo frame %zu", data.size());
            if (resolutionPending.exchange(false)) sendResolution(opt.resolutionAlias);
            uint64_t now = platform::nowMs();
            uint64_t last = lastVideoMs.exchange(now);
            if (last && now - last >= 1500) {
                XC_LOGI("picture back after %llu ms: announcing the controllers again",
                        static_cast<unsigned long long>(now - last));
                padsStale = true;
            }
            if (cb.video) cb.video(reinterpret_cast<const uint8_t*>(data.data()), data.size(), info.timestamp);
        });
    }

    uint64_t negKaMs = 0;

    void negotiateKeepalive() {
        uint64_t now = platform::nowMs();
        if (!keepaliveDue(now, negKaMs)) return;
        negKaMs = now;
        std::string e;
        if (!gssv.keepalive(e)) XC_LOGW("negoziazione keepalive: %s", e.c_str());
    }

    bool sleepUnlessStop(int ms) {
        int left = ms;
        while (left > 0) {
            if (stopRequested()) return false;
            int slice = left < kNegotiateSliceMs ? left : kNegotiateSliceMs;
            platform::sleepMs(static_cast<unsigned>(slice));
            left -= slice;
            negotiateKeepalive();
        }
        return !stopRequested();
    }

    bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (stopRequested()) {
                failed = true;
                if (failReason.empty()) failReason = "chiusura";
                return false;
            }
            if (failed) return false;
            if (pred()) return true;
            const auto now = std::chrono::steady_clock::now();
            if (now >= end) return false;
            auto slice = std::chrono::milliseconds(kNegotiateSliceMs);
            if (end - now < slice) slice = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
            cv.wait_for(lock, slice, [&] { return failed || pred() || stopRequested(); });
            lock.unlock();
            negotiateKeepalive();
            lock.lock();
        }
    }

    void addRemoteCandidate(const std::string& candidate, const std::string& mid) {
        std::string c = candidate;
        while (!c.empty() && (c.back() == ' ' || c.back() == '\r' || c.back() == '\n')) c.pop_back();
        try {
            pc->addRemoteCandidate(rtc::Candidate(c, mid));
            XC_LOGD("remote candidate added");  // not its address
        } catch (const std::exception& e) {
            XC_LOGW("ignoring a remote candidate: %s", e.what());
        }
    }

    void resetPeerForRetry() {
        try {
            if (pc) pc->close();
        } catch (...) {
        }
        pc.reset();
        chat.reset();
        control.reset();
        input.reset();
        message.reset();
        audio.reset();
        video.reset();
        videoReporter.reset();
        {
            std::lock_guard<std::mutex> lock(mutex);
            localCandidates.clear();
            gatheringDone = false;
            handshakeDone = false;
            iceUp = false;
            failed = false;
            failReason.clear();
        }
#if defined(XCLOUD_PS4)
        xc_dtls_rx_reset();
#endif
    }

    // Una negoziazione ICE+SDP. Dopo ICE connected si attende l'handshake
    // fino a connectTimeoutMs (niente chiusura PC a 3 s).
    bool negotiateOnce(std::string& err) {
        negKaMs = platform::nowMs();
        createPeer();
        if (!pc) {
            err = "PeerConnection nulla";
            return false;
        }
#if defined(XCLOUD_PS4)
        xc_dtls_rx_reset();
#endif
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("ICE_LOCAL");
#endif
        XC_LOGI("webrtc: setLocalDescription ICE");
        pc->setLocalDescription(rtc::Description::Type::Offer);
        if (!waitFor([this] { return gatheringDone; }, 10000))
            XC_LOGW("ICE gathering did not complete; continuing with %zu candidates", localCandidates.size());
        XC_LOGI("webrtc: ICE %s, %zu candidati", gatheringDone ? "completo" : "incompleto",
                localCandidates.size());

        auto local = pc->localDescription();
        if (!local) {
            err = "no local SDP offer";
            return false;
        }
        std::string offer = std::string(*local);
        XC_LOGD("local offer:\n%s", offer.c_str());
        logSdpLine("offer", offer, "a=setup:");
        logSdpLine("offer", offer, "a=fingerprint:");
        if (!gssv.sendSdpOffer(offer, err)) return false;

        std::string answer;
        for (int i = 0; i < 60 && answer.empty(); ++i) {
            if (stopRequested()) {
                err = "chiusura";
                return false;
            }
            if (!gssv.pollSdpAnswer(answer, err)) return false;
            if (answer.empty() && !sleepUnlessStop(500)) {
                err = "chiusura";
                return false;
            }
        }
        if (answer.empty()) {
            err = "timed out waiting for the SDP answer";
            return false;
        }
        XC_LOGD("remote answer:\n%s", answer.c_str());
        logSdpLine("answer", answer, "a=setup:");
        logSdpLine("answer", answer, "a=fingerprint:");
        for (size_t p = answer.find("a=rtpmap:"); p != std::string::npos; p = answer.find("a=rtpmap:", p + 1))
            XC_LOGI("answer %s", answer.substr(p, answer.find_first_of("\r\n", p) - p).c_str());
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("DTLS_REMOTE_SDP");
#endif
        XC_LOGI("webrtc: setRemoteDescription DTLS");
        try {
            pc->setRemoteDescription(rtc::Description(answer, rtc::Description::Type::Answer));
        } catch (const std::exception& e) {
            err = std::string("remote SDP rejected: ") + e.what();
            return false;
        }

        std::vector<std::string> mids = midsInOrder(offer);
        std::vector<xcloud::IceCandidate> mine;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& c : localCandidates) {
                xcloud::IceCandidate ic;
                ic.candidate = std::string(c);
                ic.sdpMid = c.mid();
                for (size_t i = 0; i < mids.size(); ++i)
                    if (mids[i] == ic.sdpMid) ic.sdpMLineIndex = static_cast<int>(i);
                mine.push_back(std::move(ic));
            }
        }
        XC_LOGI("sending %zu local ICE candidates", mine.size());
        if (!gssv.sendIceCandidates(mine, err)) return false;

        std::vector<xcloud::IceCandidate> theirs;
        for (int i = 0; i < 30 && theirs.empty(); ++i) {
            if (stopRequested()) {
                err = "chiusura";
                return false;
            }
            if (!gssv.pollIceCandidates(theirs, err)) return false;
            if (theirs.empty() && !sleepUnlessStop(1000)) {
                err = "chiusura";
                return false;
            }
        }
        if (theirs.empty()) {
            err = "the server sent no ICE candidates";
            return false;
        }
        std::vector<std::string> remoteMids = midsInOrder(answer);
        std::string bundleMid = remoteMids.empty() ? "0" : remoteMids.front();
        // Per re-iniezione se ICE ok ma DTLS 0 RX (AJ-GFN delayed-manual-candidates).
        std::string teredoIpv4;
        int teredoPort = 0;
        std::vector<std::pair<std::string, std::string>> remoteAdded;  // candidate, mid
        auto addHostPorts = [this](const std::string& ipv4, const std::vector<int>& ports, const std::string& mid,
                                   int foundationBase) {
            int f = foundationBase;
            for (int port : ports) {
                if (port <= 0) continue;
                addRemoteCandidate("a=candidate:" + std::to_string(f++) + " 1 UDP 1 " + ipv4 + " " +
                                       std::to_string(port) + " typ host",
                                   mid);
            }
        };
        for (const auto& c : theirs) {
            if (c.candidate.find("end-of-candidates") != std::string::npos) continue;
            std::string mid = c.sdpMid.empty() ? bundleMid : c.sdpMid;
            std::vector<std::string> parts;
            size_t p = 0;
            while (p < c.candidate.size()) {
                size_t e = c.candidate.find(' ', p);
                if (e == std::string::npos) e = c.candidate.size();
                parts.push_back(c.candidate.substr(p, e - p));
                p = e + 1;
            }
            std::string ipv4;
            int port = 0;
            if (parts.size() > 4 && decodeTeredo(parts[4], ipv4, port)) {
                XC_LOGI("teredo candidate -> port %d (and 9002)", port);
                teredoIpv4 = ipv4;
                teredoPort = port;
                addRemoteCandidate("a=candidate:10 1 UDP 1 " + ipv4 + " 9002 typ host", mid);
                addRemoteCandidate("a=candidate:11 1 UDP 1 " + ipv4 + " " + std::to_string(port) + " typ host", mid);
            }
            addRemoteCandidate(c.candidate, mid);
            remoteAdded.emplace_back(c.candidate, mid);
        }
        std::vector<int> answerPorts = mediaPortsFromSdp(answer);
        if (!teredoIpv4.empty() && !answerPorts.empty()) {
            XC_LOGI("dtls: porte m= answer %zu (fallback se 0 RX)", answerPorts.size());
            addHostPorts(teredoIpv4, answerPorts, bundleMid, 20);
        }

        XC_LOGI("webrtc: candidati remoti aggiunti");
        XC_LOGI("ice: attendo connected (20 s)");
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("ICE_WAIT");
#endif
        if (!waitFor([this] { return iceUp; }, 20000)) {
            std::lock_guard<std::mutex> lock(mutex);
            err = failed ? failReason : "ICE non connesso entro 20 s";
            XC_LOGE("ice: %s", err.c_str());
            return false;
        }
        XC_LOGI("ice: connected");
        try {
            rtc::Candidate localCand, remoteCand;
            if (pc && pc->getSelectedCandidatePair(&localCand, &remoteCand))
                XC_LOGI("ice: pair selezionata %s -> %s", std::string(localCand).c_str(),
                        std::string(remoteCand).c_str());
        } catch (...) {
        }
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("ICE_CONNECTED");
#endif

        // Attesa handshake fino a connectTimeoutMs. Niente riparto forzato a
        // 3 s (01.32 chiudeva la PeerConnection e riconnetteva: NEW_SEQUENCE
        // sul decoder e pad scollegato). A 3 s con DTLS 0 RX: re-iniezione
        // candidati (AJ-GFN: alcune porte rispondono ICE ma non DTLS).
        const int totalMs = opt.connectTimeoutMs > 0 ? opt.connectTimeoutMs : 30000;
        const uint64_t started = platform::nowMs();
#if defined(XCLOUD_PS4)
        bool reinjectedDtls = false;
        bool reinjectedDtlsAgain = false;
#endif
        while (platform::nowMs() - started < static_cast<uint64_t>(totalMs)) {
            if (stopRequested()) {
                err = "chiusura";
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (failed) {
                    err = failReason;
                    return false;
                }
                if (handshakeDone) {
#if defined(XCLOUD_PS4)
                    ::xc::app::trace::setStage("DTLS_OK");
#endif
                    return true;
                }
            }
#if defined(XCLOUD_PS4)
            const uint64_t elapsed = platform::nowMs() - started;
            const bool rx0 = xc_dtls_rx_pkts() == 0;
            if (!reinjectedDtls && elapsed >= static_cast<uint64_t>(kDtlsReinjectMs) && rx0) {
                reinjectedDtls = true;
                XC_LOGW("dtls: 0 byte a 3 s dopo ICE — re-iniezione candidati (niente riparto PC)");
                ::xc::app::trace::setStage("DTLS_REINJECT");
                for (const auto& rc : remoteAdded) addRemoteCandidate(rc.first, rc.second);
                if (!teredoIpv4.empty()) {
                    std::vector<int> ports = answerPorts;
                    ports.push_back(9002);
                    if (teredoPort > 0) ports.push_back(teredoPort);
                    addHostPorts(teredoIpv4, ports, bundleMid, 40);
                }
            } else if (!reinjectedDtlsAgain && elapsed >= static_cast<uint64_t>(kDtlsReinjectAgainMs) && rx0) {
                reinjectedDtlsAgain = true;
                XC_LOGW("dtls: ancora 0 byte a 8 s — seconda re-iniezione");
                ::xc::app::trace::setStage("DTLS_REINJECT2");
                for (const auto& rc : remoteAdded) addRemoteCandidate(rc.first, rc.second);
                if (!teredoIpv4.empty()) {
                    std::vector<int> ports = answerPorts;
                    ports.push_back(9002);
                    if (teredoPort > 0) ports.push_back(teredoPort);
                    addHostPorts(teredoIpv4, ports, bundleMid, 60);
                }
            }
#endif
            if (waitFor([this] { return handshakeDone; }, 200)) {
#if defined(XCLOUD_PS4)
                ::xc::app::trace::setStage("DTLS_OK");
#endif
                return true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            err = failed ? failReason : "timed out connecting the stream";
        }
#if defined(XCLOUD_PS4)
        ::xc::app::trace::setStage("DTLS_TIMEOUT");
#endif
        return false;
    }

    bool start(std::string& err) {
        // Un solo tentativo dentro questa PeerConnection. Il retry DTLS è
        // una sessione nuova (StreamPlayer), non un close a metà handshake.
        std::string localErr;
        if (negotiateOnce(localErr)) {
            lastKeepaliveMs = lastKeyframeMs = platform::nowMs();
            return true;
        }
        err = localErr;
        return false;
    }

    // Come 01.35: ack+pad nello stesso datagramma (inputReport). Gli ack
    // restano in coda finché pumpFast/sendPad (mai send dal thread video).
    void sendPad(const GamepadFrame& frame) {
        if (!input || !input->isOpen() || g_streamStop.load()) return;
        std::vector<FrameMetadata> frames;
        uint32_t seq;
        {
            std::lock_guard<std::mutex> lock(padMu);
            if (frame.index < 4) {
                lastPad[frame.index] = frame;
                lastPadMs[frame.index] = platform::nowMs();
            }
            if (pendingFrames.size() > 30)
                pendingFrames.erase(pendingFrames.begin(), pendingFrames.end() - 30);
            frames.swap(pendingFrames);
            seq = inputSequence++;
        }
        static int once = 0;
        if (!once++) XC_LOGI("input: primo pad");
        if (!frames.empty()) {
            uint64_t prev = acked.fetch_add(frames.size());
            if (prev == 0) XC_LOGI("input: ack frame");
        }
        sendInputBinary(inputReport(seq, nowMs(), &frame, frames));
    }

    void requestKeyframe() {
        json::Value v = json::Value::object();
        v.set("message", "videoKeyframeRequested");
        v.set("ifrRequested", true);
        sendText(control, v.dump());
        if (videoReporter) videoReporter->armPli();
    }

    uint64_t lastQuietKf = 0;

    void sendPadIfDue() {
        if (!input || !input->isOpen() || g_streamStop.load()) return;
        GamepadFrame f;
        f.index = 0;
        uint64_t now = platform::nowMs();
        bool due = false;
        {
            std::lock_guard<std::mutex> lock(padMu);
            due = lastPadMs[0] == 0 || now - lastPadMs[0] >= kPadRepeatMs || !pendingFrames.empty();
            if (due) f = lastPad[0];
        }
        if (due) {
            f.index = 0;
            sendPad(f);
        }
    }

    void pumpFast() {
        if (g_streamStop.load()) return;
        if (videoReporter) videoReporter->pump();
        flushOutbound();
        sendPadIfDue();
        if (videoReporter && videoReporter->secondsQuiet() >= 2) {
            uint64_t now = platform::nowMs();
            if (now - lastQuietKf >= 2000) {
                lastQuietKf = now;
                requestKeyframe();
                XC_LOGI("rtcp: video fermo da %d s, keyframe", videoReporter->secondsQuiet());
            }
        }
    }

    void tick() {
        pumpFast();
        if (g_streamStop.load()) return;
        if (control && control->isOpen() && (!padAnnounceSent || padsStale.exchange(false))) announceGamepad();
        if (!open) return;
        uint64_t now = platform::nowMs();
        if (now - lastKeepaliveMs >= 10000) {
            lastKeepaliveMs = now;
            std::string err;
            if (!gssv.keepalive(err)) XC_LOGW("%s", err.c_str());
            else {
                static int once = 0;
                if (!once++) XC_LOGI("keepalive ok");
            }
        }
        if (opt.keyframeIntervalSec > 0 && now - lastKeyframeMs >= static_cast<uint64_t>(opt.keyframeIntervalSec) * 1000u) {
            lastKeyframeMs = now;
            requestKeyframe();
        }
    }

    // Before the session goes: no callback of libdatachannel's (they hold
    // `this`) may run any more. Each reset waits for a call in progress.
    void detach() {
        {
            std::lock_guard<std::mutex> lock(guard->mutex);
            guard->self = nullptr;
        }
        try {
            for (const auto& ch : {chat, control, input, message})
                if (ch) ch->resetCallbacks();
            for (const auto& track : {audio, video})
                if (track) {
                    track->onFrame(nullptr);
                    track->resetCallbacks();
                }
            if (pc) pc->resetCallbacks();
        } catch (const std::exception& e) {
            XC_LOGW("detaching the session: %s", e.what());
        }
    }

    void close() {
        open = false;
        if (pc) {
            try {
                pc->silenceIce();
            } catch (...) {
            }
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
    // e la PS4 mostrava CE-34878-0. Niente pc->close() su questo thread.
    // halt ferma i keepalive STUN anche se _exit arriva tardi.
    if (impl_->guard) {
        std::lock_guard<std::mutex> lock(impl_->guard->mutex);
        impl_->guard->self = nullptr;
    }
    try {
        if (impl_->pc) impl_->pc->silenceIce();
    } catch (...) {
    }
    xc_log_stamp("stream: ICE halt (abandon)");
}

void StreamSession::releaseAgent() {
    impl_->closedNotified = true;
    impl_->open = false;
    if (impl_->guard) {
        std::lock_guard<std::mutex> lock(impl_->guard->mutex);
        impl_->guard->self = nullptr;
    }
    auto pc = std::move(impl_->pc);
    impl_->chat.reset();
    impl_->control.reset();
    impl_->input.reset();
    impl_->message.reset();
    impl_->audio.reset();
    impl_->video.reset();
    impl_->videoReporter.reset();
    if (!pc) {
        XC_LOGI("stream: nessun agent da chiudere");
        return;
    }
    try {
        pc->silenceIce();
    } catch (...) {
    }
    XC_LOGI("stream: ICE halt, keepalive STUN fermi");
    xc_log_stamp("stream: ICE halt");
    std::thread([pc]() mutable {
        try {
            pc->close();
        } catch (...) {
        }
        pc.reset();
        xc_log_stamp("stream: PeerConnection distrutta");
    }).detach();
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
