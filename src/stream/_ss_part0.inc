// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "stream/stream_session.h"

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

void requestStop() { g_streamStop.store(true); }

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
        if (!ch || !ch->isOpen())