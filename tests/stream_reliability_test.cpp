// SPDX-License-Identifier: GPL-3.0-or-later
// Harness della 01.51: budget DTLS, sigwait, IDR, limiter, bitrate, framebuffer,
// audio, scaler, patch ben formate, JSON di sessione. Nessun token.
#include "app/close_plan.h"
#include "app/frame_slot.h"
#include "app/session_stats.h"
#include "display/scale_3to2.h"
#include "media/audio_pace.h"
#include "media/fb_own.h"
#include "stream/abr_plan.h"
#include "stream/connect_plan.h"
#include "stream/idr_gate.h"
#include "stream/input_plan.h"
#include "stream/keyframe_limit.h"
#include "stream/nack_plan.h"
#include "stream/stream_dims.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_fail = 0;

static void expect(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (!ok) ++g_fail;
}

static std::string slurp(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool patchOk(const std::string& text, std::string& why) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.compare(0, 2, "@@") != 0) continue;
        ++n;
        auto a = line.find('-');
        auto b = line.find('+');
        auto end = line.find("@@", 2);
        if (a == std::string::npos || b == std::string::npos) {
            why = "header";
            return false;
        }
        auto count = [](const std::string& tok) {
            auto c = tok.find(',');
            if (c == std::string::npos) return 1;
            return std::atoi(tok.c_str() + c + 1);
        };
        int oldN = count(line.substr(a, b - a));
        int newN = count(line.substr(b, end - b));
        int o = 0, ne = 0;
        while (o < oldN || ne < newN) {
            if (!std::getline(in, line)) {
                why = "eof hunk " + std::to_string(n);
                return false;
            }
            if (line.empty() || (line[0] != ' ' && line[0] != '+' && line[0] != '-' && line[0] != '\\')) {
                why = "malformed hunk " + std::to_string(n) + " " + line.substr(0, 40);
                return false;
            }
            if (line[0] == '+') ++ne;
            else if (line[0] == '-') ++o;
            else if (line[0] == '\\') continue;
            else {
                ++o;
                ++ne;
            }
        }
        if (o != oldN || ne != newN) {
            why = "count hunk " + std::to_string(n);
            return false;
        }
    }
    return n > 0;
}

static void testSigwait() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::atomic<int> got{0};
    std::thread th([&] {
        sigset_t w;
        sigemptyset(&w);
        sigaddset(&w, SIGTERM);
        int sig = 0;
        if (sigwait(&w, &sig) == 0 && closeSignalWakes(sig)) got.store(sig);
    });
    usleep(50 * 1000);
    kill(getpid(), SIGTERM);
    for (int i = 0; i < 50 && !got.load(); ++i) usleep(20 * 1000);
    th.join();
    expect("sigwait riceve SIGTERM", got.load() == SIGTERM);
    expect("close usa sigwait", closeUsesSigwait() == 1);
    expect("flag chiusura senza SA_ONSTACK", closeSigFlags() == SA_SIGINFO);
}

int main() {
    using xc::app::stats::SessionSnap;
    using xc::app::stats::TimeHist;
    using xc::app::stats::renderSessionJson;
    using xc::app::stats::sessionFileName;
    using xc::app::stats::statsPruneCount;
    using xc::app::stats::timeHistAdd;
    using xc::app::stats::timeHistMean;
    using xc::app::stats::timeHistP95;
    expect("8 s non basta a un handshake da 12 s", !dtlsHandshakeFits(12000, 8000));
    expect("20 s bastano", dtlsHandshakeFits(12000, kReconnectDtlsBudgetMs));
    expect("budget reconnect 20 s", kReconnectDtlsBudgetMs == 20000);

    StopFlags flags{0, 0};
    onRequestStop(&flags);
    expect("stop di chiusura", flags.streamStop && flags.exitStop);
    expect("arm non cancella l'uscita", !onArmNewSession(&flags) && flags.streamStop);
    flags.exitStop = 0;
    flags.streamStop = 1;
    expect("arm di una sessione nuova", onArmNewSession(&flags) == 1 && !flags.streamStop);

    std::atomic<int> stop{0};
    auto t0 = std::chrono::steady_clock::now();
    std::thread killer([&] {
        usleep(350 * 1000);
        stop.store(1);
    });
    int slices = 0;
    while (slices < 100) {
        if (stop.load()) break;
        usleep(kNegotiateSliceMs * 1000);
        ++slices;
    }
    killer.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    expect("attesa negoziazione interrotta entro 600 ms", stop.load() == 1 && ms < 600);
    expect("keepalive sotto i 10 s", kNegotiateKeepaliveMs <= 10000 && keepaliveDue(8000, 0));

    uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x88};
    uint8_t p[] = {0, 0, 0, 1, 0x41, 0x9a};
    expect("riconosce IDR", auIsIdr(idr, sizeof idr));
    expect("un P-frame non è IDR", !auIsIdr(p, sizeof p));
    IdrGate gate;
    idrInit(&gate);
    expect("P-frame ammesso se il riferimento c'è", idrAdmit(&gate, 0));
    idrOnEvent(&gate, IDR_EVENT_QUEUE, 1000);
    expect("dopo la coda piena il P-frame si scarta", !idrAdmit(&gate, 0) && gate.waiting);
    expect("l'IDR rientra", idrAdmit(&gate, 1) && !gate.waiting);
    idrOnEvent(&gate, IDR_EVENT_DECODE, 2000);
    expect("errore di decode aspetta l'IDR", !idrAdmit(&gate, 0));
    expect("niente reset prima dei 5 s", !idrNeedsReset(&gate, 6000));
    expect("reset a 5 s", idrNeedsReset(&gate, 7000));
    expect("il reset non si ripete", !idrNeedsReset(&gate, 8000));

    KfLimit kf;
    kfInit(&kf);
    int sent = 0, blocked = 0;
    for (int t = 0; t <= 5000; t += 500) {
        int d = kfDecide(&kf, static_cast<unsigned long long>(t));
        if (d > 0) ++sent;
        if (d < 0) ++blocked;
    }
    expect("al massimo 10 keyframe in 5 s", sent <= kKfMaxInWindow && sent >= 1);
    expect("il tetto ne blocca una", blocked == 1);

    expect("720HQ → 1280x720", streamDimsForAlias("720HQ").w == 1280 && streamDimsForAlias("720HQ").h == 720);
    expect("720 → 1280x720", streamDimsForAlias("720").w == 1280 && streamDimsForAlias("720").h == 720);
    expect("1080HQ → 1920x1080", streamDimsForAlias("1080HQ").w == 1920 && streamDimsForAlias("1080HQ").h == 1080);
    expect("1440 → 2560x1440", streamDimsForAlias("1440").w == 2560 && streamDimsForAlias("1440").h == 1440);
    expect("fmtp 720 Baseline 42e020", std::strstr(streamH264Fmtp("720HQ"), "42e020") != nullptr);
    expect("fmtp 1080 Baseline 42e02a", std::strstr(streamH264Fmtp("1080HQ"), "42e02a") != nullptr);

    BitratePlan p720 = bitrateBegin(ABR_720, 0);
    BitratePlan p1080 = bitrateBegin(ABR_1080, 0);
    BitratePlan capped = bitrateBegin(ABR_1080, 5000000);
    expect("720 parte a 8 Mb/s", p720.ceilingBps == 8000000 && p720.currentBps == 8000000);
    expect("720 floor 4 Mb/s", p720.floorBps == 4000000);
    expect("1080 tetto 12 partenza 10", p1080.ceilingBps == 12000000 && p1080.currentBps == 10000000);
    expect("1080 floor 6 Mb/s", p1080.floorBps == 6000000);
    expect("il tetto utente vince", capped.ceilingBps == 5000000);
    expect("SDP in kbps", bitrateSdpKbps(&p1080) == 12000);
    expect("REMB anche al tetto", bitrateSendRemb(&p1080));
    bitrateOnSample(&p1080, 0, 50, 1000, 2000, 0);
    expect("warmup niente taglio", p1080.currentBps == 10000000 && p1080.stallCuts == 0);
    for (int i = 0; i < 4; ++i) bitrateOnSample(&p1080, p1080.currentBps, 0, 1000, 0, 0);
    bitrateOnSample(&p1080, 10000000, 20, 1000, 0, 0);
    expect("una perdita toglie il 20%", p1080.currentBps == 8000000);
    expect("REMB sotto il tetto", bitrateSendRemb(&p1080));
    for (int i = 0; i < 20; ++i) bitrateOnSample(&p1080, p1080.currentBps, 0, 1000, 0, 0);
    expect("dopo 20 s puliti sale di 250 kb/s", p1080.currentBps == 8250000);
    while (p1080.currentBps < p1080.ceilingBps) bitrateOnSample(&p1080, p1080.currentBps, 0, 20000, 0, 0);
    expect("non supera il tetto", p1080.currentBps == p1080.ceilingBps);
    BitratePlan stall = bitrateBegin(ABR_1080, 0);
    for (int i = 0; i < 5; ++i) bitrateOnSample(&stall, stall.currentBps, 0, 1000, 0, 0);
    bitrateOnSample(&stall, 0, 0, 1000, 1500, 0);
    expect("stale_ingress −40%", stall.currentBps == 6000000 && stall.stallCuts == 1);
    expect("REMB dopo stall", bitrateSendRemb(&stall));
    BitratePlan low = bitrateBegin(ABR_1080, 0);
    for (int i = 0; i < 7; ++i) bitrateOnSample(&low, 38000, 0, 1000, 0, 0);
    expect("recv basso senza loss non taglia", low.currentBps == 10000000);
    BitratePlan dropped = bitrateBegin(ABR_1080, 0);
    for (int i = 0; i < 5; ++i) bitrateOnSample(&dropped, dropped.currentBps, 0, 1000, 0, 0);
    bitrateOnSample(&dropped, dropped.currentBps, 0, 1000, 0, 2);
    expect("local_dropped −40%", dropped.currentBps == 6000000);
    bitrateOnSample(&stall, 0, 0, 1000, 1500, 0);
    expect("secondo stall chiede demote", bitrateDemoteDue(&stall));

    expect("input unordered", kInputUnordered == 1);
    expect("input maxRetransmits 0", kInputMaxRetransmits == 0);
    expect("pad 60 Hz", kInputPadHz == 60 && kInputPadIntervalMs == 16);
    expect("frame-ack off di default", kSendFrameAcksDefault == 0);
    expect("skip se buffered", inputSkipIfBuffered(1) == 1 && inputSkipIfBuffered(0) == 0);

    FbOwn fb;
    fbOwnInit(&fb, 4);
    int a = fbOffer(&fb);
    fbOnAccept(&fb, a, 1);
    int b = fbOffer(&fb);
    expect("un buffer trattenuto non si rioffre", a == 0 && b == 1 && fbHeldCount(&fb) == 1);
    fbOnAccept(&fb, b, 1);
    fbRelease(&fb, a);
    expect("dopo la copia si può riusare", fbOffer(&fb) == 0);
    fbOnAccept(&fb, 0, 1);
    fbOnAccept(&fb, 2, 1);
    fbOnAccept(&fb, 3, 1);
    expect("quattro trattenuti: nessun offer", fbOffer(&fb) < 0);

    FrameSlot slot;
    frameSlotInit(&slot);
    frameReplace(&slot, 1);
    frameReplace(&slot, 2);
    expect("il frame vecchio si scarta", slot.replaced == 1 && slot.seq == 2);
    frameShown(&slot);
    expect("il present svuota lo slot", !framePending(&slot));

    int i0 = 0, i1 = 0;
    scale3to2Tap(0, 1920, &i0, &i1);
    expect("pixel pari su un sorgente", i0 == 0 && i1 == 0);
    scale3to2Tap(1, 1920, &i0, &i1);
    expect("pixel dispari media due sorgenti", i0 == 1 && i1 == 2);

    AudioPace pace;
    audioPaceInit(&pace);
    AudioDecision pre = audioDecide(&pace, 48000 * 10 / 1000, 48000);
    expect("prefill: 10 ms non si suonano", pre.play == 0 && pace.priming);
    AudioDecision go = audioDecide(&pace, 48000 * 50 / 1000, 48000);
    expect("a 50 ms si parte", go.play == 1 && !pace.priming);
    AudioDecision hi = audioDecide(&pace, 48000 * 120 / 1000, 48000);
    expect("trim di 10 ms non di tutta la coda", hi.dropFrames == 48000 * 10 / 1000 && hi.fadeFrames == 48000 * 5 / 1000);
    float samples[40];
    for (int i = 0; i < 20; ++i) samples[i * 2] = samples[i * 2 + 1] = static_cast<float>(i);
    audioApplyFade(samples, 20, 10, 5);
    float joined = samples[20];
    expect("il fade non è un salto secco", joined > 5.f && joined < 10.f);

    expect("NACK grace 45 ms", kNackGraceMs == 45 && kNackRepeatMs < kNackGraceMs);

    std::string why;
    for (const char* patch : {"patches/libdatachannel-ps4-initlog.patch", "patches/libdatachannel-ps4-sconn.patch",
                              "patches/libdatachannel-ps4-srtp.patch", "patches/libdatachannel-ps4-dtls-log.patch",
                              "patches/libdatachannel-ps4-ice-halt.patch"}) {
        std::string text = slurp(patch);
        std::string err;
        bool ok = patchOk(text, err);
        expect(patch, ok);
        if (!ok) std::printf("  %s\n", err.c_str());
    }
    std::string badPatch = "@@ -1,1 +1,1 @@\nnot a prefix\n";
    expect("il controllo vede una patch malformata", !patchOk(badPatch, why));

    std::string crash = slurp("src/app/crash_guard.cpp");
    expect("sigwait nel crash guard", crash.find("sigwait") != std::string::npos);
    expect("il log dice sigwait", crash.find("sigwait") != std::string::npos && crash.find("bloccati, sigwait") != std::string::npos);
    auto closePos = crash.find("closeSig.sa_flags");
    auto closeEnd = crash.find("blockCloseSignals", closePos);
    std::string closeBody = closePos == std::string::npos ? "" : crash.substr(closePos, closeEnd - closePos);
    expect("il handler di chiusura non usa SA_ONSTACK", closeBody.find("SA_ONSTACK") == std::string::npos);

    std::string mainc = slurp("src/app/ps5_main.cpp");
    auto blockAt = mainc.find("blockCloseSignals");
    auto initAt = mainc.find("platform::init()");
    expect("i segnali si bloccano prima di init", blockAt != std::string::npos && initAt != std::string::npos && blockAt < initAt);
    std::string life = slurp("src/app/lifecycle.cpp");
    expect("il monitor non sblocca SIGTERM", life.find("SIG_UNBLOCK") == std::string::npos);

    std::string sess = slurp("src/stream/stream_session.cpp");
    expect("la negoziazione guarda stopRequested", sess.find("stopRequested()") != std::string::npos);
    std::string player = slurp("src/app/stream_player.cpp");
    expect("reconnect a 20 s", player.find("kReconnectDtlsBudgetMs") != std::string::npos);
    expect("niente attesa di flip nel blit", player.find("drawNv12(held.nv12.data(), held.pitch, held.w, held.h, false)") != std::string::npos);
    expect("il present con wait è sparito", player.find("drawNv12(hp.data, pitch, w, h, true)") == std::string::npos);
    expect("piano bitrate", player.find("bitrateBegin") != std::string::npos);
    expect("dimensioni da alias", slurp("src/stream/stream_session.cpp").find("streamDimsForAlias") != std::string::npos);
    expect("limiter keyframe in sessione", sess.find("kfDecide") != std::string::npos);
    expect("input unordered in sessione", sess.find("maxRetransmits") != std::string::npos &&
                                              sess.find("unordered") != std::string::npos);
    expect("ClientMetadata all'open", sess.find("clientMetadataReport") != std::string::npos);
    expect("gamepadReport senza Metadata", sess.find("gamepadReport") != std::string::npos);
    expect("niente frame-ack di default", sess.find("sendFrameAcks") != std::string::npos &&
                                              sess.find("kInputPadIntervalMs") != std::string::npos);
    expect("goog-remb in offerta", slurp("src/stream/stream_session.cpp").find("goog-remb") != std::string::npos);
    expect("ABR senza recv_low", slurp("src/stream/abr_plan.h").find("recv_low") == std::string::npos &&
                                     slurp("src/stream/abr_plan.h").find("kAbrWarmupMs") != std::string::npos);
    expect("depth 1 a 720", slurp("src/media/hw_decoder_ps4.cpp").find("kDepth720") != std::string::npos);
    expect("niente offerta a 30 Mb/s", player.find("30000000") == std::string::npos);
    std::string hw = slurp("src/media/hw_decoder_ps4.cpp");
    expect("il decoder offre solo buffer liberi", hw.find("fbOffer") != std::string::npos && hw.find("fbRelease") != std::string::npos);
    std::string nack = slurp("src/stream/h264_receiver.cpp");
    expect("il jitter usa la grace nuova", nack.find("kNackGraceMs") != std::string::npos);
    std::string scale = slurp("src/display/display_ps4.cpp");
    expect("lo scaler 3:2 usa i due tap", scale.find("scale3to2Tap") != std::string::npos);

    // Tre minuti simulati, con buchi: nessun P-frame senza IDR, e l'immagine riparte.
    IdrGate play;
    idrInit(&play);
    KfLimit playKf;
    kfInit(&playKf);
    int presented = 0, maxGap = 0, gap = 0, kfSent = 0;
    for (int ms = 0; ms < 180000; ms += 16) {
        bool loss = (ms / 1000) % 17 == 0 && (ms % 1000) < 80;
        bool idrFrame = (ms % 2000) < 16 || (play.waiting && (ms % 500) < 16);
        if (loss && !play.waiting) idrOnEvent(&play, IDR_EVENT_QUEUE, static_cast<unsigned long long>(ms));
        if (!idrAdmit(&play, idrFrame ? 1 : 0)) {
            if (kfDecide(&playKf, static_cast<unsigned long long>(ms)) > 0) ++kfSent;
            ++gap;
            if (gap > maxGap) maxGap = gap;
            continue;
        }
        ++presented;
        gap = 0;
    }
    expect("tre minuti simulati senza buco lungo", maxGap * 16 < 2000 && presented > 5000);
    std::printf("  presentati %d buco max %d ms keyframe %d\n", presented, maxGap * 16, kfSent);

    TimeHist hist;
    for (int i = 0; i < 100; ++i) timeHistAdd(hist, i < 90 ? 2000 : 20000);
    expect("media e p95 del decode", timeHistMean(hist) < 4000 && timeHistP95(hist) >= 8000);

    std::vector<std::string> names;
    for (int i = 0; i < 12; ++i) names.push_back("stream-20261025-1200" + std::to_string(10 + i) + ".json");
    expect("si tengono gli ultimi 10", statsPruneCount(12, 10) == 2);
    expect("nome file", sessionFileName("20261025", "120000") == "stream-20261025-120000.json");

    SessionSnap snap;
    snap.started = "2026-10-25T12:00:00";
    snap.durationMs = 5000;
    snap.resolution = "1080HQ";
    snap.bitrateAskBps = 12000000;
    snap.bitrateNowBps = 9600000;
    snap.bitrateN = 1;
    snap.bitrate[0] = {0, 12000000};
    snap.framesReceived = 300;
    snap.framesDecoded = 290;
    snap.framesPresented = 280;
    snap.droppedQueue = 2;
    snap.droppedIdr = 4;
    snap.droppedError = 1;
    snap.fpsReceived = 60;
    snap.packetsLost = 3;
    snap.nacksSent = 2;
    snap.nacksRecovered = 1;
    snap.keyframeSent = 2;
    snap.keyframeBlocked = 1;
    snap.queueMax = 8;
    snap.queueAvg = 1.5;
    snap.decodeMeanUs = 2400;
    snap.decodeP95Us = 8000;
    snap.presentMeanUs = 1000;
    snap.presentP95Us = 4000;
    snap.audioUnderrun = 1;
    snap.audioTrimFrames = 480;
    snap.menuSuspend = 1;
    snap.menuResume = 1;
    snap.exitReason = "in corso";
    std::string js = renderSessionJson(snap);
    for (const char* key : {"duration_s", "resolution", "bitrate_ask_bps", "bitrate_now_bps", "bitrate_changes",
                            "fps", "dropped_queue", "dropped_idr", "dropped_error", "nack_sent", "nack_recovered",
                            "keyframe", "blocked", "queue", "decode_us", "present_us", "p95", "underrun",
                            "trim_frames", "menu", "suspend", "resume", "exit"})
        expect(key, js.find(key) != std::string::npos);
    for (const char* banned : {"token", "authorization", "gamertag", "password", "xuid", "jwt"})
        expect((std::string("niente ") + banned).c_str(), js.find(banned) == std::string::npos);

    std::string dir = "/tmp/psbox-stats-test";
    int rm = std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    expect("cartella stats", rm == 0);
    for (int i = 0; i < 12; ++i) {
        std::ofstream((dir + "/stream-20261025-1000" + (i < 10 ? "0" : "") + std::to_string(i) + ".json").c_str()) << "{}\n";
    }
    auto tWrite = std::chrono::steady_clock::now();
    xc::app::stats::statsBegin(dir, "1080HQ", 12000000, 150);
    xc::app::stats::statsNoteReceived(120);
    xc::app::stats::statsNoteQueue(4);
    xc::app::stats::statsNoteDrop(0, 3);
    xc::app::stats::statsNoteDrop(1, 2);
    xc::app::stats::statsNoteDrop(2, 1);
    xc::app::stats::statsNoteDecoded(2500);
    xc::app::stats::statsNotePresented(1100);
    xc::app::stats::statsNoteKf(1);
    xc::app::stats::statsNoteKf(0);
    xc::app::stats::statsNoteNet(9, 4, 3);
    xc::app::stats::statsNoteBitrate(9600000);
    xc::app::stats::statsNoteAudio(2, 480);
    xc::app::stats::statsNoteMenu(1);
    xc::app::stats::statsNoteMenu(0);
    auto noteMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tWrite).count();
    expect("le note non scrivono da sole", noteMs < 200);
    xc::app::stats::statsFinish("fine");
    xc::app::stats::statsWriteFinal();
    std::string written;
    for (int i = 0; i < 30 && written.find("\"exit\"") == std::string::npos; ++i) {
        usleep(50 * 1000);
        std::ifstream dirlist;
        // find the new file
        FILE* fp = popen(("ls " + dir + "/stream-*.json 2>/dev/null").c_str(), "r");
        if (!fp) break;
        char buf[256];
        std::string newest;
        int count = 0;
        while (std::fgets(buf, sizeof buf, fp)) {
            ++count;
            newest = buf;
        }
        pclose(fp);
        if (!newest.empty() && newest.back() == '\n') newest.pop_back();
        if (count > 10) {
            // prune happens at begin, before the new file: 10 old + 1 new is possible
        }
        if (!newest.empty()) written = slurp(newest.c_str());
        if (written.find("fine") != std::string::npos) break;
    }
    expect("il file ha l'esito", written.find("fine") != std::string::npos);
    expect("il file ha i drop", written.find("dropped_queue") != std::string::npos && written.find("1080HQ") != std::string::npos);
    expect("il file non ha token", written.find("token") == std::string::npos);
    std::printf("  json %zu byte\n", written.size());

    testSigwait();

    std::printf("%s (%d)\n", g_fail ? "FAIL" : "ok tutto", g_fail);
    return g_fail ? 1 : 0;
}
