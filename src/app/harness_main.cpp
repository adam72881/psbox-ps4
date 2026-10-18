// SPDX-License-Identifier: GPL-3.0-or-later
// Headless Linux xCloud harness: same protocol stack as the PS4 app
// (MSA refresh → GSSV → WebRTC → input/control/message → frame ack → RTCP).
// Decodes H.264 with FFmpeg (no display). Used to reproduce the ~10 s RTP
// stop and prove a 10-minute session before porting fixes to the console.
#include "auth/auth_manager.h"
#include "media/decoder.h"
#include "net/http.h"
#include "platform/platform.h"
#include "stream/stream_session.h"
#include "util/log.h"
#include "xcloud/gssv.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace xc;

namespace {

bool looksLikeJson(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i < s.size() && s[i] == '{';
}

bool loadAccountJson(std::string& pathOut) {
    std::string text;
    if (const char* env = std::getenv("PSBOX_ACCOUNT_JSON"); env && *env) {
        std::string v(env);
        if (looksLikeJson(v)) {
            text = std::move(v);
        } else {
            // Env may hold a path (e.g. ~/.psbox/account.json); never log it.
            if (!platform::readFile(v, text) || text.empty()) {
                XC_LOGE("harness: PSBOX_ACCOUNT_JSON path unreadable");
                return false;
            }
        }
    } else {
        const char* home = std::getenv("HOME");
        std::string p = std::string(home ? home : ".") + "/.psbox/account.json";
        if (!platform::readFile(p, text) || text.empty()) {
            XC_LOGE("harness: set PSBOX_ACCOUNT_JSON or put account at ~/.psbox/account.json");
            return false;
        }
    }
    if (!looksLikeJson(text)) {
        XC_LOGE("harness: account store is not JSON");
        return false;
    }
    pathOut = platform::dataDir() + "/account.json";
    if (!platform::writeFileAtomic(pathOut, text)) {
        XC_LOGE("harness: cannot write account store");
        return false;
    }
    ::chmod(pathOut.c_str(), 0600);
    return true;
}

bool signIn(auth::AuthManager& am, xcloud::GssvClient& gssv) {
    std::string err;
    auto show = [](const auth::DeviceCode& dc) {
        std::printf("device code: open %s enter %s\n", dc.verificationUri.c_str(), dc.userCode.c_str());
        std::fflush(stdout);
    };
    if (!am.signIn(gssv, show, err)) {
        XC_LOGE("sign-in: %s", err.c_str());
        return false;
    }
    XC_LOGI("harness: signed in as %s region %s", am.profile().gamertag.c_str(), gssv.region().name.c_str());
    return true;
}

bool provision(auth::AuthManager& am, xcloud::GssvClient& gssv, const std::string& titleId) {
    std::string err;
    if (!gssv.startSession(titleId, "en-US", err)) {
        XC_LOGE("%s", err.c_str());
        return false;
    }
    bool connected = false;
    for (int i = 0; i < 600; ++i) {
        xcloud::SessionStatus st;
        if (!gssv.sessionState(st, err)) {
            XC_LOGE("%s", err.c_str());
            return false;
        }
        if (st.state == xcloud::SessionState::WaitingForResources)
            XC_LOGI("queue wait ~%ds", gssv.waitTimeSeconds());
        else
            XC_LOGI("session: %s", st.raw.c_str());
        if (st.state == xcloud::SessionState::Failed) {
            XC_LOGE("session failed: %s %s", st.errorCode.c_str(), st.errorMessage.c_str());
            return false;
        }
        if (st.state == xcloud::SessionState::ReadyToConnect && !connected) {
            std::string transfer;
            if (!am.consoleTransferToken(transfer, err) || !gssv.connect(transfer, err)) {
                XC_LOGE("%s", err.c_str());
                return false;
            }
            connected = true;
        }
        if (st.state == xcloud::SessionState::Provisioned) return true;
        platform::sleepMs(1000);
    }
    XC_LOGE("provision timeout");
    return false;
}

struct Au {
    std::vector<uint8_t> data;
    uint32_t rtpTs = 0;
    double arrival = 0;
};

struct Counters {
    std::atomic<uint64_t> videoFrames{0}, decoded{0}, decodeFail{0}, audioPkts{0};
    std::atomic<uint64_t> acks{0};
    std::atomic<uint64_t> queued{0}, dropped{0};
    std::atomic<bool> closed{false};
    std::string closeWhy;
    std::mutex mu;
};

int runStream(auth::AuthManager& am, const std::string& titleId, int seconds, bool doAck, bool doPad,
              xcloud::Resolution res, const char* forceAlias = nullptr) {
    xcloud::GssvClient gssv;
    gssv.setResolution(res);
    if (!signIn(am, gssv)) return 2;
    if (!provision(am, gssv, titleId)) {
        gssv.stopSession();
        return 3;
    }

    Counters c;
    media::VideoDecoder decoder;
    if (!decoder.init(2)) {
        XC_LOGE("ffmpeg H.264 decoder init failed");
        gssv.stopSession();
        return 4;
    }

    // RTP/juice thread only enqueues AUs. Decode + reportFrame run on the
    // main loop — sending on the input DC from the RTP callback freezes SCTP
    // (reproduced: Fortnite hung after the first frame when ack was inline).
    constexpr size_t kMaxQueued = 48;
    std::mutex qMu;
    std::deque<Au> q;

    stream::StreamSession* sess = nullptr;
    stream::StreamCallbacks cb;
    cb.video = [&](const uint8_t* d, size_t n, uint32_t rtpTs) {
        if (n == 0) return;
        double arrival = stream::clockMs();
        ++c.videoFrames;
        {
            std::lock_guard<std::mutex> lock(qMu);
            if (q.size() >= kMaxQueued) {
                while (q.size() > 8) {
                    q.pop_front();
                    ++c.dropped;
                }
            }
            q.push_back({std::vector<uint8_t>(d, d + n), rtpTs, arrival});
            c.queued.store(q.size());
        }
    };
    cb.audio = [&](const uint8_t*, size_t, uint32_t) { ++c.audioPkts; };
    cb.closed = [&](const std::string& why, bool) {
        {
            std::lock_guard<std::mutex> lock(c.mu);
            c.closeWhy = why;
        }
        c.closed = true;
        XC_LOGW("harness: session closed: %s", why.c_str());
    };

    stream::StreamOptions opts;
    opts.maxBitrate = res == xcloud::Resolution::P720      ? 12000000u
                      : res == xcloud::Resolution::P1080HQ ? 30000000u
                                                           : 20000000u;
    // Come PSBox PS5 stream_player.
    if (forceAlias) opts.resolutionAlias = forceAlias;
    else if (res == xcloud::Resolution::P720) opts.resolutionAlias = "720HQ";
    else if (res == xcloud::Resolution::P1080 || res == xcloud::Resolution::P1080HQ)
        opts.resolutionAlias = "1080HQ";
    else opts.resolutionAlias = "";
    stream::StreamSession session(gssv, cb, opts);
    sess = &session;

    std::string err;
    if (!session.start(err)) {
        XC_LOGE("stream start: %s", err.c_str());
        gssv.stopSession();
        return 5;
    }
    XC_LOGI("harness: streaming %s for %ds (ack=%d pad=%d)", titleId.c_str(), seconds, doAck ? 1 : 0,
            doPad ? 1 : 0);

    const uint64_t t0 = platform::nowMs();
    const uint64_t end = t0 + static_cast<uint64_t>(seconds) * 1000u;
    uint64_t next = t0 + 1000;
    uint64_t silentSec = 0;
    uint64_t lastFrames = 0;
    int rc = 0;
    stream::GamepadFrame pad;
    pad.index = 0;
    // Idle xCloud (~8–10 min senza input “reale”): un tap A periodico evita
    // serverInitiatedDisconnect / KickForBeingIdle (visto a t≈517s).
    uint64_t nextIdleTap = t0 + 45000;
    bool tapDown = false;

    auto drainVideo = [&](int maxFrames) {
        for (int n = 0; n < maxFrames; ++n) {
            Au au;
            {
                std::lock_guard<std::mutex> lock(qMu);
                if (q.empty()) return;
                au = std::move(q.front());
                q.pop_front();
                c.queued.store(q.size());
            }
            media::Picture pic;
            bool ok = decoder.decode(au.data.data(), au.data.size(), pic, au.rtpTs);
            if (!ok) {
                ++c.decodeFail;
                continue;
            }
            ++c.decoded;
            if (doAck && sess) {
                stream::FrameMetadata meta;
                meta.serverDataKey = au.rtpTs;
                meta.firstPacketArrivalMs = au.arrival;
                meta.submittedMs = au.arrival;
                meta.decodedMs = stream::clockMs();
                meta.renderedMs = meta.decodedMs;
                sess->reportFrame(meta);
                ++c.acks;
            }
        }
    };

    while (platform::nowMs() < end && !c.closed.load()) {
        // Pochi AU per giro: lascia tempo a pumpFast (flush message/ack/pad).
        drainVideo(3);
        uint64_t now = platform::nowMs();
        if (doPad) {
            if (now >= nextIdleTap) {
                tapDown = !tapDown;
                if (tapDown) pad.buttons |= stream::kA;
                else pad.buttons &= static_cast<uint16_t>(~stream::kA);
                if (!tapDown) nextIdleTap = now + 60000;  // un tap/min
                else nextIdleTap = now + 80;              // 80 ms premuto
            }
            session.sendGamepad(pad);
        }
        session.pumpFast();
        if (now >= next) {
            next += 1000;
            session.tick();
            const auto& rtp = session.videoStats();
            uint64_t frames = c.videoFrames.load();
            uint64_t delta = frames - lastFrames;
            lastFrames = frames;
            if (delta == 0) ++silentSec;
            else silentSec = 0;
            const uint64_t decodedNow = c.decoded.load();
            static uint64_t lastDecoded = 0;
            const unsigned fps = static_cast<unsigned>(decodedNow - lastDecoded);
            lastDecoded = decodedNow;
            const char* stability =
                delta == 0 ? "STALL" : (fps < 45 || delta < 45 ? "LOW" : (fps > 75 ? "HIGH" : "OK"));
            std::printf(
                "t=%llis fps=%u video=%llu decoded=%llu fail=%llu ack=%llu audio=%llu q=%llu drop=%llu | "
                "rtp=%llu kbps remb=%u lost=%llu rtt=%d silent=%llus open=%d stab=%s\n",
                static_cast<long long>((now - t0) / 1000), fps,
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(decodedNow),
                static_cast<unsigned long long>(c.decodeFail.load()),
                static_cast<unsigned long long>(session.framesAcked()),
                static_cast<unsigned long long>(c.audioPkts.load()),
                static_cast<unsigned long long>(c.queued.load()),
                static_cast<unsigned long long>(c.dropped.load()),
                static_cast<unsigned long long>(rtp.receiveRate.load() / 1000), rtp.estimate.load() / 1000,
                static_cast<unsigned long long>(rtp.lost.load()), session.rttMs(),
                static_cast<unsigned long long>(silentSec), session.isOpen() ? 1 : 0, stability);
            std::fflush(stdout);
            if (silentSec >= 8 && frames > 30) {
                XC_LOGE("harness: RTP silent for %llu s after %llu frames — FAIL",
                        static_cast<unsigned long long>(silentSec),
                        static_cast<unsigned long long>(frames));
                rc = 10;
                break;
            }
        }
        platform::sleepMs(4);
    }

    drainVideo(64);

    if (c.closed.load()) {
        std::lock_guard<std::mutex> lock(c.mu);
        XC_LOGE("harness: closed early: %s", c.closeWhy.c_str());
        if (rc == 0) rc = 11;
    } else if (rc == 0 && c.decoded.load() == 0) {
        rc = 12;
    } else if (rc == 0) {
        XC_LOGI("harness: OK %llu decoded, %llu acks in %ds",
                static_cast<unsigned long long>(c.decoded.load()),
                static_cast<unsigned long long>(c.acks.load()), seconds);
    }

    XC_LOGI("harness: teardown begin");
    const uint64_t tear0 = platform::nowMs();
    session.close();
    XC_LOGI("harness: session closed in %llu ms",
            static_cast<unsigned long long>(platform::nowMs() - tear0));
    gssv.stopSession();
    const uint64_t tear1 = platform::nowMs() - tear0;
    XC_LOGI("harness: teardown done in %llu ms rc=%d", static_cast<unsigned long long>(tear1), rc);
    if (rc == 0 && tear1 > 15000) {
        XC_LOGE("harness: teardown took too long (%llu ms) — FAIL",
                static_cast<unsigned long long>(tear1));
        rc = 13;
    }
    return rc;
}

void usage(const char* argv0) {
    std::printf(
        "Usage: %s stream <titleId> [--seconds=N] [--no-ack] [--no-pad]\n"
        "          [--720p|--1080hq|--force-alias=1080HQ]\n"
        "       %s titles\n"
        "Env: PSBOX_ACCOUNT_JSON (JSON or path) or ~/.psbox/account.json\n",
        argv0, argv0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    {
        std::string data = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.psbox/harness";
        setenv("XCLOUD_DATA_DIR", data.c_str(), 1);
    }
    if (!platform::init()) return 1;
    {
        std::string logPath = platform::dataDir() + "/harness.log";
        xc::log::setFile(logPath.c_str());
    }
    if (!net::initTls(platform::caBundlePath())) {
        XC_LOGE("TLS init failed: %s (CA %s)", net::tlsInitError().c_str(), platform::caBundlePath().c_str());
        return 1;
    }

    std::string store;
    if (!loadAccountJson(store)) return 1;
    auth::AuthManager am(store);

    std::string cmd = argv[1];
    if (cmd == "titles") {
        xcloud::GssvClient gssv;
        if (!signIn(am, gssv)) return 2;
        std::vector<xcloud::Title> titles;
        std::string err;
        if (!gssv.listTitles(titles, err, true)) {
            XC_LOGE("%s", err.c_str());
            return 2;
        }
        for (const auto& t : titles)
            std::printf("%s\t%s\n", t.titleId.c_str(), t.name.empty() ? "?" : t.name.c_str());
        return 0;
    }
    if (cmd == "stream" && argc >= 3) {
        std::string title = argv[2];
        int seconds = 60;
        bool ack = true, pad = true;
        xcloud::Resolution res = xcloud::Resolution::P1080;
        const char* forceAlias = nullptr;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--seconds=", 0) == 0) seconds = std::atoi(a.c_str() + 10);
            else if (a == "--no-ack") ack = false;
            else if (a == "--no-pad") pad = false;
            else if (a == "--720p") res = xcloud::Resolution::P720;
            else if (a == "--1080hq") res = xcloud::Resolution::P1080HQ;
            else if (a.rfind("--force-alias=", 0) == 0) forceAlias = argv[i] + 14;
        }
        if (seconds < 5) seconds = 5;
        int rc = runStream(am, title, seconds, ack, pad, res, forceAlias);
        platform::shutdown();
        return rc;
    }
    usage(argv[0]);
    return 1;
}
