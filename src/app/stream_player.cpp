// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "app/stream_player.h"

#include "app/crash_guard.h"
#include "app/frame_slot.h"
#include "app/session_stats.h"
#include "display/display.h"
#include "input/controller.h"
#include "media/audio_out.h"
#include "media/audio_in.h"
#include "media/audio_encoder.h"
#include "media/decoder.h"
#include "media/hw_decoder.h"
#include "platform/platform.h"
#include "stream/abr_plan.h"
#include "stream/connect_plan.h"
#include "stream/idr_gate.h"
#include "stream/stream_session.h"
#include "stream/frame_queue.h"
#include "stream/recv_diag.h"
#include "ui/strings.h"
#include "util/log.h"

#if defined(XCLOUD_PS4)
#include "app/ps5_app.h"
#include "app/screenshot.h"
#include "app/stream_trace.h"
#include "audio_orbis.h"
#include <chrono>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

extern "C" void xc_log_stamp(const char* msg);

namespace xc::app {

namespace {
// Come il loop video della PS5: oltre questa coda si riparte da un keyframe.
// Il jitter buffer può consegnare diversi frame insieme dopo una ritrasmissione.
constexpr size_t kMaxQueuedFrames = 30;
#if !defined(XCLOUD_PS4)
constexpr int kMaxFullStreak = 30;  // frames shown one refresh late before dropping one
#endif
}

struct StreamPlayer::Impl {
    xcloud::GssvClient& gssv;
    // The WebRTC session: replaced when the connection drops and the stream
    // reconnects (worker thread), read by the video, input and main threads.
    mutable std::mutex sessionMutex;
    std::shared_ptr<stream::StreamSession> session;
    std::shared_ptr<stream::StreamSession> current() const {
        std::lock_guard<std::mutex> lock(sessionMutex);
        return session;
    }
    stream::StreamCallbacks callbacks;
    stream::StreamOptions options;
    // Reconnecting after a lost connection, as the xbox.com client does: up
    // to 20 attempts a second apart while the cloud session is still there.
    std::atomic<bool> reconnecting{false};
    std::atomic<bool> needDecoderReset{false};
    std::atomic<bool> waitIdr{false};  // dopo Reset o perdita di riferimento: solo IDR
    std::atomic<uint64_t> idrSinceMs{0};
    int idrResetDone = 0;
    int preferredTier = ABR_1080;
    int wantPromote1080 = 0;
    int tierSwapArmed = 0;
    FrameSlot shownSlot{};
    uint64_t picSeq = 0;
    struct HeldPic {
        bool valid = false;
        std::vector<uint8_t> nv12;
        int pitch = 0, w = 0, h = 0;
        uint32_t rtp = 0;
        double arrivalMs = 0, submittedMs = 0, decodedMs = 0;
        uint64_t arrivalUs = 0;
    };
    HeldPic held;
    std::atomic<bool> titleFocused{true};
    int reconnectAttempts = 0;
    std::atomic<uint32_t> reconnects{0};
    uint64_t lastRtpPackets = 0, lastAudioPackets = 0;
    int silentTicks = 0;

    std::mutex mutex;
    std::condition_variable cv;
    using VideoFrame = stream::QueuedFrame;
    stream::FrameQueue<32> frameQ;
    std::atomic<bool> running{false};
    std::string endReason;
    platform::Thread videoThread, audioThread;
#if defined(XCLOUD_PS4)
    bool orbisAudio = false;
    media::HwDecoder hw;
#endif
    std::deque<std::vector<uint8_t>> audioPackets_;
    std::condition_variable audioCv;

    media::AudioDecoder audioDecoder;
    std::vector<float> pcm;
    std::atomic<uint64_t> videoFrames{0}, decodedFrames{0}, droppedFrames{0}, audioPackets{0};
    std::atomic<uint64_t> decodeFailures{0}, queueResets{0}, keyframeRequests{0};
    std::atomic<bool> keyframeWanted{false};
    std::atomic<uint32_t> pictureSize{0};  // width << 16 | height
    std::atomic<uint64_t> vibrations{0}, lateFrames{0};
    std::atomic<uint64_t> triggerVibrations{0};
    std::atomic<uint64_t> displayUs{0}, displayCount{0}, displayMaxUs{0};
    std::atomic<uint64_t> decodeUs{0}, decodeCalls{0}, decodeMaxUs{0}, drawUs{0}, drawCalls{0}, drawMaxUs{0};
    int decodeThreads = 1;
    int simulatedLoss = 0;
    int maxBitrateMbps = 0;
    bool hwProbe = false;

    static void atomicMax(std::atomic<uint64_t>& a, uint64_t v) {
        uint64_t cur = a;
        while (v > cur && !a.compare_exchange_weak(cur, v)) {
        }
    }
    bool loggedFirstDecode = false;
    FILE* dumpFile = nullptr;
    uint64_t dumpUntilMs = 0;
    std::mutex snapshotMutex;
    std::string snapshotPath;
    std::string screenPath;  // video thread only
    uint64_t lastKeyframeRequestMs = 0;
    // The game's text requests not yet taken, and those it withdrew (mutex).
    std::deque<stream::TextInputRequest> textRequests;
    std::set<std::string> textWithdrawn;

    media::AudioIn audioIn;
    media::AudioEncoder audioEncoder;
    std::atomic<bool> micEnabled{true};
    std::atomic<bool> micMuted{false};
    std::atomic<float> micGain{1.0f};
    std::atomic<uint32_t> audioSendTimestamp{0};
    bool encoderInitialized = false;
    // Opening and closing the microphone happen on several threads (start,
    // reconnect, the end of the stream): one at a time.
    std::mutex micMutex;

    void closeMic() {
        std::lock_guard<std::mutex> lock(micMutex);
        audioIn.close();
        input::setEmbeddedMicActive(false);
    }

    void updateMicState() {
        std::lock_guard<std::mutex> lock(micMutex);
        if (!running) return;
        if (!micEnabled) {
            if (audioIn.isRecording()) {
                audioIn.close();
                input::setEmbeddedMicActive(false);
            }
            return;
        }
        if (!audioIn.isRecording()) {
            if (!encoderInitialized) {
                encoderInitialized = audioEncoder.init(24000);
                if (!encoderInitialized) {
                    XC_LOGW("audio encoder: Opus init failed, mic input & meter will still operate");
                }
            }
            audioIn.setMuted(micMuted.load());
            audioIn.setGain(micGain.load());
            audioIn.open([this](const int16_t* pcmFrames, size_t samples) {
                if (!running) return;
                if (!encoderInitialized) return;
                auto s = current();
                if (!s || !s->isOpen()) return;
                std::vector<uint8_t> opus;
                if (audioEncoder.encode(pcmFrames, samples, opus)) {
                    uint32_t ts = audioSendTimestamp.fetch_add(static_cast<uint32_t>(samples));
                    s->sendAudio(opus.data(), opus.size(), ts);
                }
            });
            input::setEmbeddedMicActive(true);
        } else {
            audioIn.setMuted(micMuted.load());
            audioIn.setGain(micGain.load());
        }
    }

    explicit Impl(xcloud::GssvClient& g) : gssv(g) {}

    void end(const std::string& reason) {
        closeMic();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (endReason.empty()) endReason = reason;
        }
        running = false;
        cv.notify_all();
        audioCv.notify_all();
        psbox_rd_stop_logger();
        const char* why = "fine";
        if (reason.find("lost") != std::string::npos || reason.find("persa") != std::string::npos ||
            reason.find("couldn't") != std::string::npos)
            why = "persa";
        stats::statsFinish(why);
#if defined(XCLOUD_PS4)
        trace::setStage(("END " + reason).c_str());
        trace::endSession();
#endif
    }

    void armReferenceLoss() {
        if (!waitIdr.exchange(true)) idrSinceMs.store(platform::nowMs());
        keyframeWanted = true;
    }

    void audioLoop() {
        std::vector<uint8_t> packet;
        bool first = true;
        while (running) {
            {
                std::unique_lock<std::mutex> lock(mutex);
#if defined(XCLOUD_PS4)
                if (orbisAudio) {
                    // Nessun pacchetto: PLC di audio_orbis (sampleData nullo).
                    if (!audioCv.wait_for(lock, std::chrono::milliseconds(20),
                                          [&] { return !running || !audioPackets_.empty(); })) {
                        if (!running || first) continue;
                        lock.unlock();
                        audio_callbacks_orbis.decodeAndPlaySample(nullptr, 0);
                        continue;
                    }
                } else
#endif
                    audioCv.wait(lock, [&] { return !running || !audioPackets_.empty(); });
                if (!running) break;
                packet = std::move(audioPackets_.front());
                audioPackets_.pop_front();
            }
#if defined(XCLOUD_PS4)
            if (orbisAudio) {
                audio_callbacks_orbis.decodeAndPlaySample(reinterpret_cast<char*>(packet.data()),
                                                          static_cast<int>(packet.size()));
                if (first) {
                    first = false;
                    XC_LOGI("primo pacchetto Opus verso audio_orbis (%zu byte)", packet.size());
                }
                continue;
            }
#endif
            pcm.clear();
            if (audioDecoder.decode(packet.data(), packet.size(), pcm)) media::audioPush(pcm.data(), pcm.size() / 2);
            if (first) {
                first = false;
                XC_LOGI("first audio packet decoded: %zu samples", pcm.size() / 2);
            }
        }
    }

    void videoLoop() {
#if defined(XCLOUD_PS4)
        // Su PS4 non esiste FFmpeg: lo stub software logga e ritorna, e il
        // thread non deve proseguire. Il decoder è Videodec2, creato in start().
        if (!hw.ready()) {
            std::string why = hw.lastError().empty() ? "Videodec2 non partito" : hw.lastError();
            XC_LOGE("video: %s", why.c_str());
            end(why);
            return;
        }
        XC_LOGI("video: decodifica solo con Videodec2");
        xc_log_stamp("video: loop avviato, attendo AU");
        trace::setStage("VIDEO_LOOP");
        bool loggedAu = false, loggedDecode = false, loggedBlit = false;
        uint64_t lastTraceSec = platform::nowMs();
        uint64_t presentedAtSec = 0, lateAtSec = 0;
        frameSlotInit(&shownSlot);
        auto presentHeld = [&] {
            if (!held.valid || g_psMenu.load() || g_appStopping.load()) return;
            std::unique_lock<std::timed_mutex> lock(display::frameMutex(), std::defer_lock);
            if (!lock.try_lock()) return;
            uint64_t tDraw0 = platform::nowUs();
            if (!loggedBlit) {
                XC_LOGI("video: primo blit %dx%d pitch %d", held.w, held.h, held.pitch);
                trace::setStage("PRESENT_FIRST");
            }
            // wait=false: un flip occupato non blocca il decode.
            bool drawn = display::drawNv12(held.nv12.data(), held.pitch, held.w, held.h, false);
            bool shown = drawn && display::present();
            uint64_t presentUs = platform::nowUs() - tDraw0;
            if (!loggedBlit) {
                loggedBlit = true;
                XC_LOGI("video: primo blit %s", shown ? "ok" : "in coda");
            }
            constexpr int kDstW = 1280, kDstH = 720;
            trace::notePresent(decodedFrames, held.w, held.h, kDstW, kDstH, presentUs, shown);
            if (!shown) return;
            stats::statsNotePresented(presentUs);
            frameShown(&shownSlot);
            stream::FrameMetadata meta;
            meta.serverDataKey = held.rtp;
            meta.firstPacketArrivalMs = held.arrivalMs ? held.arrivalMs : held.submittedMs;
            meta.submittedMs = held.submittedMs;
            meta.decodedMs = held.decodedMs;
            meta.renderedMs = stream::clockMs();
            if (auto s = current()) s->reportFrame(meta);
            if (decodedFrames == 1) {
                XC_LOGI("primo frame Videodec2 presentato %dx%d", held.w, held.h);
                if (shot::enabled()) {
                    shot::request(shot::Kind::FirstStream, true);
                    shot::afterPresent();
                }
            }
            drawUs += presentUs;
            ++drawCalls;
            ++presentedAtSec;
            atomicMax(drawMaxUs, presentUs);
            uint64_t lat = held.arrivalUs && platform::nowUs() > held.arrivalUs ? platform::nowUs() - held.arrivalUs : presentUs;
            displayUs += lat;
            ++displayCount;
            atomicMax(displayMaxUs, lat);
            held.valid = false;
        };
        while (running && !g_appStopping.load()) {
            crash::videoHeartbeat();
            presentHeld();
            VideoFrame frame;
            bool have = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (frameQ.sizeApprox() == 0) {
                    cv.wait_for(lock, std::chrono::milliseconds(held.valid ? 8 : 500),
                                [&] { return !running || g_appStopping.load() || frameQ.sizeApprox() != 0; });
                }
                if (!running || g_appStopping.load()) break;
                if (frameQ.sizeApprox() != 0) {
                    if (!frameQ.tryPop(frame)) continue;
                    have = true;
                }
            }
            crash::videoHeartbeat();
            if (!have) continue;
            if (frame.data.empty()) {
                XC_LOGW("video: AU vuota");
                continue;
            }
            if (!loggedAu) {
                loggedAu = true;
                XC_LOGI("video: prima AU %zu byte", frame.data.size());
                xc_log_stamp("video: prima AU");
            }
            uint64_t nowMs = platform::nowMs();
            if (waitIdr.load() && idrSinceMs.load() && !idrResetDone && nowMs - idrSinceMs.load() > kIdrResetAfterMs) {
                idrResetDone = 1;
                needDecoderReset.store(true);
            }
            if (needDecoderReset.exchange(false)) {
                XC_LOGI("video: Reset decoder (nuova sessione/sequenza) — attendo IDR");
                hw.reset();
                if (!waitIdr.exchange(true)) idrSinceMs.store(nowMs);
                keyframeWanted = true;
                requestKeyframeIfWanted();
            }
            if (g_appStopping.load()) break;
            if (g_psMenu.load()) {
                size_t n = 1;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    n += frameQ.sizeApprox();
                    frameQ.clear();
                }
                stats::statsNoteDrop(1, n);
                if (!waitIdr.exchange(true)) idrSinceMs.store(platform::nowMs());
                keyframeWanted = true;
                requestKeyframeIfWanted();
                platform::sleepMs(40);
                continue;
            }
            if (waitIdr.load()) {
                keyframeWanted = true;
                requestKeyframeIfWanted();
            }
            if (waitIdr.load() && !auIsIdr(frame.data.data(), frame.data.size())) {
                stats::statsNoteDrop(1, 1);
                continue;
            }
            if (waitIdr.exchange(false)) {
                idrSinceMs.store(0);
                idrResetDone = 0;
                XC_LOGI("video: IDR dopo la perdita di riferimento, riprendo Decode");
            }
            media::HwDecoder::Picture hp;
            bool got = false;
            double submittedMs = stream::clockMs();
            uint64_t tDec0 = platform::nowUs();
            try {
                if (!loggedDecode) {
                    XC_LOGI("video: prima Decode");
                    xc_log_stamp("video: Decode enter");
                    trace::setStage("DECODE_FIRST");
                }
                got = hw.decode(frame.data.data(), frame.data.size(), frame.rtpTimestamp, hp);
                uint64_t took = platform::nowUs() - tDec0;
                decodeUs += took;
                ++decodeCalls;
                atomicMax(decodeMaxUs, took);
                crash::videoHeartbeat();
                if (!loggedDecode) {
                    loggedDecode = true;
                    xc_log_stamp(got ? "video: Decode leave ok" : "video: Decode leave no-pic");
                    XC_LOGI("video: prima Decode %s %ux%u", got ? "ok" : "senza immagine", hp.width, hp.height);
                }
            } catch (const std::exception& e) {
                std::string why = std::string("Decode Videodec2: ") + e.what();
                XC_LOGE("video: %s", why.c_str());
                if (g_ui) g_ui->showError(why);
                end(why);
                return;
            } catch (...) {
                XC_LOGE("video: eccezione Videodec2");
                if (g_ui) g_ui->showError("Decode Videodec2: errore interno");
                end("Decode Videodec2: errore interno");
                return;
            }
            if (!got || !hp.data || hp.width < 2 || hp.height < 2) {
                if (hw.lastFailed()) {
                    ++decodeFailures;
                    stats::statsNoteDrop(2, 1);
                    if (hw.lastNeededReset()) needDecoderReset.store(true);
                    if (!waitIdr.exchange(true)) idrSinceMs.store(platform::nowMs());
                    if (decodeFailures <= 8 || decodeFailures % 50 == 0) {
                        uint8_t nal = 0;
                        if (frame.data.size() >= 5 && frame.data[0] == 0 && frame.data[1] == 0 &&
                            frame.data[2] == 0 && frame.data[3] == 1)
                            nal = frame.data[4] & 0x1F;
                        XC_LOGW("video: decode fail #%llu nal %u AU %zu %s",
                                static_cast<unsigned long long>(decodeFailures), static_cast<unsigned>(nal),
                                frame.data.size(), hw.lastError().c_str());
                    }
                    keyframeWanted = true;
                    requestKeyframeIfWanted();
                } else {
                    static int waiting = 0;
                    if (!waiting++) XC_LOGI("video: decoder in attesa della prima immagine");
                }
                continue;
            }
            stats::statsNoteDecoded(platform::nowUs() - tDec0);
            int w = static_cast<int>(hp.width) & ~1;
            int h = static_cast<int>(hp.height) & ~1;
            int pitch = hp.pitchBytes ? static_cast<int>(hp.pitchBytes) : static_cast<int>(hp.width);
            if (pitch < w) pitch = w;
            size_t need = static_cast<size_t>(pitch) * static_cast<size_t>(hp.height) * 3 / 2;
            if (hp.size < need) {
                XC_LOGE("video: frame corto %zu < %zu", hp.size, need);
                ++decodeFailures;
                stats::statsNoteDrop(2, 1);
                if (!waitIdr.exchange(true)) idrSinceMs.store(platform::nowMs());
                keyframeWanted = true;
                requestKeyframeIfWanted();
                continue;
            }
            ++decodedFrames;
            pictureSize = (static_cast<uint32_t>(w) << 16) | static_cast<uint32_t>(h);
            if (shownSlot.seq) stats::statsNoteDrop(3, 1);
            frameReplace(&shownSlot, ++picSeq);
            held.nv12.assign(hp.data, hp.data + need);
            held.pitch = pitch;
            held.w = w;
            held.h = h;
            held.rtp = frame.rtpTimestamp;
            held.arrivalMs = frame.arrivalMs;
            held.submittedMs = submittedMs;
            held.decodedMs = stream::clockMs();
            held.arrivalUs = frame.arrivalUs;
            held.valid = true;
            uint64_t now = platform::nowMs();
            if (now - lastTraceSec >= 1000) {
                lastTraceSec = now;
                trace::setStage("STREAMING");
                trace::tickSecond(presentedAtSec, lateAtSec, decodeFailures.load(), videoFrames.load());
                presentedAtSec = 0;
                lateAtSec = 0;
            }
        }
        trace::setStage("VIDEO_LOOP_EXIT");
        return;
#else
        media::VideoDecoder decoder;
        VideoFrame frame;
        std::vector<uint8_t>& au = frame.data;
        // Timings of the frames handed to the decoder, by RTP timestamp:
        // pictures can come out later than their input.
        std::deque<stream::FrameMetadata> inFlight;
        media::HwDecoder hw;
        bool hwOk = hwProbe && hw.init(1920, 1088);
        bool swOk = decoder.init(decodeThreads);
        if (!swOk) {
#if defined(XCLOUD_PS4)
            if (!hwOk) {
                end("could not start the H.264 decoder");
                return;
            }
            XC_LOGW("decoder software assente: le immagini passano da Videodec2");
#else
            end("could not start the H.264 decoder");
            return;
#endif
        }
        if (hwProbe) XC_LOGI("hwdec: probe %s", hwOk ? "running" : "failed to start");
        uint64_t hwUs = 0, hwMaxUs = 0, hwPictures = 0, hwCalls = 0, hwLogAt = platform::nowMs() + 1000;
        int fullStreak = 0;
        int skippedInRow = 0;
        constexpr uint64_t kShownRing = 8;
        std::pair<uint64_t, uint64_t> inScreenQueue[kShownRing] = {};  // present id, arrival us
        uint64_t lastShownId = 0;
        while (running) {
            size_t backlog;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return !running || frameQ.sizeApprox() != 0; });
                if (!running) break;
                if (!frameQ.tryPop(frame)) continue;
                backlog = frameQ.sizeApprox();
            }
            if (dumpFile) {
                // 4-byte little-endian length, then the access unit as received.
                uint32_t n = static_cast<uint32_t>(au.size());
                std::fwrite(&n, 4, 1, dumpFile);
                std::fwrite(au.data(), 1, au.size(), dumpFile);
                if (platform::nowMs() >= dumpUntilMs) {
                    std::fclose(dumpFile);
                    dumpFile = nullptr;
                    XC_LOGI("video dump complete");
                }
            }
            media::Picture pic;
            const bool first = decodedFrames == 0 && !loggedFirstDecode;
            if (first) XC_LOGI("first video decode: %zu bytes", au.size());
            if (hwOk) {
                media::HwDecoder::Picture hp;
                uint64_t h0 = platform::nowUs();
                bool gotHw = hw.decode(au.data(), au.size(), frame.rtpTimestamp, hp);
                if (gotHw) ++hwPictures;
                uint64_t took = platform::nowUs() - h0;
                hwUs += took;
                hwMaxUs = std::max(hwMaxUs, took);
                ++hwCalls;
                if (platform::nowMs() >= hwLogAt) {
                    hwLogAt += 1000;
                    XC_LOGI("hwdec: %llu calls, %llu pictures, %.2f/%.2f ms", static_cast<unsigned long long>(hwCalls),
                            static_cast<unsigned long long>(hwPictures), hwCalls ? hwUs / 1000.0 / hwCalls : 0.0,
                            hwMaxUs / 1000.0);
                    hwUs = hwMaxUs = hwCalls = hwPictures = 0;
                }
#if defined(XCLOUD_PS4)
                // Senza FFmpeg il frame hardware (NV12) è l'unica immagine.
                if (!swOk && gotHw && hp.data && hp.width > 1 && hp.height > 1) {
                    int w = static_cast<int>(hp.width) & ~1;
                    int h = static_cast<int>(hp.height) & ~1;
                    int pitch = hp.pitchBytes ? static_cast<int>(hp.pitchBytes)
                                              : static_cast<int>(hp.pitch ? hp.pitch : hp.width);
                    const uint8_t* srcY = hp.data;
                    const uint8_t* srcUV = hp.data + static_cast<size_t>(pitch) * hp.height;
                    static std::vector<uint8_t> uPlane, vPlane;
                    uPlane.resize(static_cast<size_t>(w / 2) * static_cast<size_t>(h / 2));
                    vPlane.resize(uPlane.size());
                    for (int y = 0; y < h / 2; ++y) {
                        const uint8_t* row = srcUV + static_cast<size_t>(y) * pitch;
                        for (int x = 0; x < w / 2; ++x) {
                            uPlane[static_cast<size_t>(y) * (w / 2) + x] = row[x * 2];
                            vPlane[static_cast<size_t>(y) * (w / 2) + x] = row[x * 2 + 1];
                        }
                    }
                    std::lock_guard<std::timed_mutex> lock(display::frameMutex());
                    if (display::drawYuv420(srcY, uPlane.data(), vPlane.data(), pitch, w / 2, w / 2, w, h, true)) {
                        display::present();
                        ++decodedFrames;
                        pictureSize = (static_cast<uint32_t>(w) << 16) | static_cast<uint32_t>(h);
                    }
                }
                if (!swOk) continue;
#endif
            }
#if defined(XCLOUD_PS4)
            if (!swOk) continue;
#endif
            stream::FrameMetadata meta;
            meta.serverDataKey = frame.rtpTimestamp;
            meta.firstPacketArrivalMs = frame.arrivalMs;
            meta.submittedMs = stream::clockMs();
            inFlight.push_back(meta);
            if (inFlight.size() > 16) inFlight.pop_front();
            uint64_t t0 = platform::nowUs();
            bool ok = decoder.decode(au.data(), au.size(), pic, frame.rtpTimestamp);
            uint64_t t1 = platform::nowUs();
            bool known = false;
            if (ok) {
                for (auto it = inFlight.begin(); it != inFlight.end(); ++it)
                    if (it->serverDataKey == pic.tag) {
                        meta = *it;
                        inFlight.erase(inFlight.begin(), it + 1);  // and anything older: never coming
                        known = true;
                        break;
                    }
                meta.decodedMs = stream::clockMs();
            }
            decodeUs += t1 - t0;
            ++decodeCalls;
            atomicMax(decodeMaxUs, t1 - t0);
            if (first) {
                loggedFirstDecode = true;
                XC_LOGI("first video decode done: ok=%d", ok);
            }
            if (decoder.needsKeyframe()) keyframeWanted = true;
            requestKeyframeIfWanted();
            if (!ok) {
                if (!decoder.pending()) ++decodeFailures;
                continue;
            }
            ++decodedFrames;
            pictureSize = (static_cast<uint32_t>(pic.width) << 16) | static_cast<uint32_t>(pic.height);
            bool screenshot = saveSnapshotIfAsked(pic);
            // A newer frame already waits: decode this one (the next needs it)
            // but show the newer one, a frame sooner. At most two in a row, so
            // the picture keeps moving when decoding runs behind.
            if (backlog > 0 && skippedInRow < 2) {
                ++skippedInRow;
                ++droppedFrames;
                continue;
            }
            skippedInRow = 0;
            std::lock_guard<std::timed_mutex> lock(display::frameMutex());
            if (decodedFrames == 1) XC_LOGI("first draw %dx%d", pic.width, pic.height);
            uint64_t t2 = platform::nowUs();
            // Both flips queued means the picture shows a frame later than
            // it could. Arrival jitter does that now and then; it is waited
            // out (the queue then stays full: stream and display both run at
            // 60 Hz). After half a second of that, a frame is dropped to get
            // the latency back.
            bool drawn = display::drawYuv420(pic.y, pic.u, pic.v, pic.strideY, pic.strideU, pic.strideV,
                                             pic.width, pic.height, false);
            if (drawn) {
                fullStreak = 0;
            } else if (++fullStreak < kMaxFullStreak) {
                drawn = display::drawYuv420(pic.y, pic.u, pic.v, pic.strideY, pic.strideU, pic.strideV,
                                            pic.width, pic.height, true);
            } else {
                fullStreak = 0;
            }
            if (!drawn) {
                ++lateFrames;
                continue;
            }
            if (!display::present()) {
                ++lateFrames;
                continue;
            }
            uint64_t t3 = platform::nowUs();
            // Display latency: network arrival to the TV, once the GPU says
            // the frame is on the screen (a frame or so later).
            if (uint64_t id = display::lastPresentId()) {
                inScreenQueue[id % kShownRing] = {id, frame.arrivalUs};
                uint64_t shownId = 0, shownAt = 0;
                if (display::lastShown(shownId, shownAt) && shownId != lastShownId) {
                    lastShownId = shownId;
                    const auto& q = inScreenQueue[shownId % kShownRing];
                    if (q.first == shownId && shownAt > q.second) {
                        uint64_t lat = shownAt - q.second;
                        displayUs += lat;
                        ++displayCount;
                        atomicMax(displayMaxUs, lat);
                    }
                }
            }
            meta.renderedMs = stream::clockMs();
            if (auto s = current(); s && known) s->reportFrame(meta);
            if (decodedFrames % 600 == 1)
                XC_LOGI("frame %u: queued %.1f, decoded %.1f, shown %.1f ms after arrival", meta.serverDataKey,
                        meta.submittedMs - meta.firstPacketArrivalMs, meta.decodedMs - meta.firstPacketArrivalMs,
                        meta.renderedMs - meta.firstPacketArrivalMs);
            if (screenshot) saveScreen();
            drawUs += t3 - t2;
            ++drawCalls;
            atomicMax(drawMaxUs, t3 - t2);
            if (decodedFrames == 1) XC_LOGI("first frame presented");
        }
#endif
    }


    bool start(std::string& err) {
#if defined(XCLOUD_PS4)
        // Stereo Opus 48 kHz, come l'SDP (minptime=10, stereo=1). 960 campioni = 20 ms.
        OPUS_MULTISTREAM_CONFIGURATION opus{};
        opus.sampleRate = 48000;
        opus.channelCount = 2;
        opus.streams = 1;
        opus.coupledStreams = 1;
        opus.samplesPerFrame = 960;
        opus.mapping[0] = 0;
        opus.mapping[1] = 1;
        orbisAudio = audio_callbacks_orbis.init(0, &opus, nullptr, 0) == 0;
        if (!orbisAudio) XC_LOGW("audio_orbis non partito: lo stream resta senza audio");
        XC_LOGI("video: init Videodec2, niente software");
        // 720 → depth 1 e buffer 1280x720; 1080/1440 → depth 2 e 1920x1088.
        {
            int hwW = 1920, hwH = 1088;
            if (gssv.resolution() == xcloud::Resolution::P720) {
                hwW = 1280;
                hwH = 720;
            }
            if (!hw.init(hwW, hwH)) {
                err = hw.lastError().empty() ? "Videodec2 non partito" : hw.lastError();
                XC_LOGE("video: %s", err.c_str());
                return false;
            }
        }
        trace::beginSession();
        psbox_rd_reset();
        trace::setStage("PLAYER_START");
#endif
        if (
#if defined(XCLOUD_PS4)
            !orbisAudio &&
#endif
            !audioDecoder.init())
            XC_LOGW("no audio decoder: playing without sound");
        if (
#if defined(XCLOUD_PS4)
            !orbisAudio &&
#endif
            !media::audioStart())
            XC_LOGW("no audio output: playing without sound");

        stream::StreamCallbacks cb;
        cb.video = [this](const uint8_t* d, size_t n, uint32_t rtpTimestamp) {
            // Thread RTP/juice: coda lock-free, drop se piena. Niente mutex/decoder/send.
            if (n == 0) {
                keyframeWanted = true;
                return;
            }
            if (++videoFrames == 1) {
                XC_LOGI("first video frame received: %zu bytes", n);
                xc_log_stamp("video: rtp→coda lock-free");
            }
            {
                static int lastProfile = -1, lastLevel = -1;
                for (size_t i = 0; i + 6 < n; ++i)
                    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1F) == 7) {
                        int profile = d[i + 4], level = d[i + 6];
                        if (profile != lastProfile || level != lastLevel) {
                            lastProfile = profile;
                            lastLevel = level;
                            XC_LOGI("video: H.264 profile %d (%s), level %d.%d", profile,
                                    profile == 66 ? "Baseline" : profile == 77 ? "Main" : profile == 100 ? "High" : "?",
                                    level / 10, level % 10);
                        }
                        break;
                    }
            }
            VideoFrame vf;
            vf.data.assign(d, d + n);
            vf.rtpTimestamp = rtpTimestamp;
            vf.arrivalMs = stream::clockMs();
            vf.arrivalUs = platform::nowUs();
            if (!frameQ.tryPush(std::move(vf))) {
                frameQ.clear();
                droppedFrames += 1;
                ++queueResets;
                keyframeWanted = true;
                waitIdr.store(true);
                if (!idrSinceMs.load()) idrSinceMs.store(platform::nowMs());
                stats::statsNoteDrop(0, 1);
                // retry once after clear
                VideoFrame again;
                again.data.assign(d, d + n);
                again.rtpTimestamp = rtpTimestamp;
                again.arrivalMs = stream::clockMs();
                again.arrivalUs = platform::nowUs();
                frameQ.tryPush(std::move(again));
            }
            stats::statsNoteReceived(1);
            stats::statsNoteQueue(static_cast<uint32_t>(frameQ.sizeApprox()));
            cv.notify_one();
        };
        cb.audio = [this](const uint8_t* d, size_t n, uint32_t) {
            if (++audioPackets == 1) XC_LOGI("first audio packet: %zu bytes", n);
            std::lock_guard<std::mutex> lock(mutex);
            if (audioPackets_.size() < 50) audioPackets_.emplace_back(d, d + n);
            audioCv.notify_one();
        };
        // xCloud's motors are 0..100 percent, like the web client's
        // dual-rumble effect (left = strong, right = weak). The trigger
        // motors (impulse triggers) become the adaptive triggers vibrating.
        cb.vibration = [this](const stream::Vibration& v) {
            // The first few, and the first few with the triggers (rarer: does
            // the service send them at all for a game?).
            bool triggers = v.leftTrigger || v.rightTrigger;
            if (++vibrations <= 5 || (triggers && ++triggerVibrations <= 5))
                XC_LOGI("vibration %u/%u/%u/%u for %ums (pad %u)", v.leftMotor, v.rightMotor, v.leftTrigger,
                        v.rightTrigger, v.durationMs, v.gamepadIndex);
            auto scale = [](uint8_t pct) { return static_cast<uint8_t>(std::min<int>(pct, 100) * 255 / 100); };
            // Il DS4 non ha i grilletti impulsivi: la vibrazione dei grilletti
            // si somma ai due motori, altrimenti il rumble del gioco non si sente.
            uint8_t left = scale(v.leftMotor), right = scale(v.rightMotor);
            uint8_t lt = scale(v.leftTrigger), rt = scale(v.rightTrigger);
            if (lt > left) left = lt;
            if (rt > right) right = rt;
            input::setRumble(left, right, v.durationMs ? v.durationMs : 200, v.gamepadIndex);
            input::setTriggerRumble(lt, rt, v.durationMs, v.gamepadIndex);
        };
        cb.titleFocus = [this](bool focused) {
            if (titleFocused.exchange(focused) != focused) XC_LOGI("title %s the focus", focused ? "has" : "lost");
        };
        cb.idleWarning = [](int seconds) {
            platform::notify(ui::trf(ui::Str::IdleWarning, std::to_string(seconds)));
        };
        cb.closed = [this](const std::string& reason, bool recoverable) {
            // A lost connection: the worker's tick() reconnects. The server
            // ending the session (or no game left) ends the stream.
            if (recoverable && running) {
                if (!reconnecting.exchange(true)) XC_LOGW("connection lost (%s): reconnecting", reason.c_str());
            } else {
                end(reason);
            }
        };
        // With the console's keyboard, the game's text fields use it;
        // otherwise the server draws the Xbox keyboard into the picture.
        if (platform::systemKeyboardAvailable()) {
            cb.textInput = [this](const stream::TextInputRequest& req) {
                std::lock_guard<std::mutex> lock(mutex);
                textRequests.push_back(req);
            };
            cb.textInputCancelled = [this](const std::string& id) {
                std::lock_guard<std::mutex> lock(mutex);
                textWithdrawn.insert(id);
            };
        }

        stream::StreamOptions opts;
        // Each setting asks for the best tier of its resolution, like the
        // xbox.com client: the HQ tiers (higher bitrate) and 1440p need Game
        // Pass Ultimate and a market where Microsoft enabled them; elsewhere
        // the plain tier the session asks for first holds.
        // Come PSBox PS5: 1080 e 1080HQ chiedono entrambi il tier "1080HQ".
        int tier = ABR_1080;
        switch (gssv.resolution()) {
        case xcloud::Resolution::P1440:
            opts.resolutionAlias = "1440";
            tier = ABR_1440;
            break;
        case xcloud::Resolution::P1080:
        case xcloud::Resolution::P1080HQ:
            /* XCloud4 1080p-G8: alias 1080, 1920x1080, 12000 kbps. */
            opts.resolutionAlias = "1080";
            tier = ABR_1080;
            break;
        case xcloud::Resolution::P720:
            opts.resolutionAlias = "720HQ";
            tier = ABR_720;
            break;
        }
        preferredTier = tier;
        BitratePlan plan = bitrateBegin(tier, maxBitrateMbps > 0 ? static_cast<uint32_t>(maxBitrateMbps) * 1000000u : 0);
        opts.maxBitrate = plan.ceilingBps;
        opts.abrTier = tier;
        opts.simulatedVideoLoss = simulatedLoss;
        XC_LOGI("stream: tier %s tetto %u kbps floor %u kbps", opts.resolutionAlias.c_str(), plan.ceilingBps / 1000u,
                plan.floorBps / 1000u);
        if (const char* loss = std::getenv("XC_SIM_LOSS")) opts.simulatedVideoLoss = std::atoi(loss);
        callbacks = cb;
        options = opts;
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            session = std::make_shared<stream::StreamSession>(gssv, cb, opts);
        }
        running = true;
        XC_LOGI("stream: creo thread video (stack 8 MiB)");
        if (!platform::startThread(videoThread, [this] { videoLoop(); }, 8u << 20)) {
            err = "thread video non creato";
            XC_LOGE("stream: %s", err.c_str());
            running = false;
            return false;
        }
        XC_LOGI("stream: thread audio (stack 8 MiB)");
        if (!platform::startThread(audioThread, [this] { audioLoop(); }, 8u << 20)) {
            err = "thread audio non creato";
            XC_LOGE("stream: %s", err.c_str());
            running = false;
            return false;
        }
        if (g_appStopping.load()) {
            err = "chiusura";
            running = false;
            return false;
        }
        stream::armNewSession();
        stats::statsBegin(platform::dataDir() + "/stats", opts.resolutionAlias.c_str(), opts.maxBitrate, stats::kStatsIntervalMs);
        // Prima di start(): se negotiateOnce resta in attesa handshake, il
        // worker non chiama tick() — i contatori devono uscire lo stesso.
        psbox_rd_start_logger();
        XC_LOGI("stream: avvio PeerConnection");
        auto dropSession = [](std::shared_ptr<stream::StreamSession> failed) {
            if (!failed) return;
            try {
                failed->releaseAgent();
            } catch (...) {
            }
        };
        auto tryStart = [&](std::shared_ptr<stream::StreamSession> s) {
            err.clear();
            if (!s || !s->start(err)) {
                if (err.empty()) err = "PeerConnection non avviata";
                return false;
            }
            return true;
        };
        if (!tryStart(current())) {
            bool dtlsTimeout = err.find("timed out connecting the stream") != std::string::npos;
            XC_LOGE("stream: PeerConnection: %s", err.c_str());
            // Non requestStop: quel flag è globale e il tentativo dopo
            // usciva subito con «chiusura» (01.46, secondo avvio Fortnite).
            // end() solo a fallimento definitivo: spegne i thread audio/video.
            std::shared_ptr<stream::StreamSession> failed;
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                failed = std::move(session);
            }
            dropSession(std::move(failed));
            if (dtlsTimeout && !g_appStopping.load() && running) {
                XC_LOGW("stream: timeout DTLS, un nuovo tentativo con PeerConnection nuova");
                stream::armNewSession();
                auto next = std::make_shared<stream::StreamSession>(gssv, callbacks, options);
                {
                    std::lock_guard<std::mutex> lock(sessionMutex);
                    session = next;
                }
                if (tryStart(next)) {
                    XC_LOGI("stream: PeerConnection avviata (retry DTLS)");
                    updateMicState();
                    return true;
                }
                XC_LOGE("stream: retry DTLS: %s", err.c_str());
                {
                    std::lock_guard<std::mutex> lock(sessionMutex);
                    if (session == next) session.reset();
                }
                dropSession(std::move(next));
            }
            end(err);
            reconnecting = false;
            cv.notify_all();
            audioCv.notify_all();
            return false;
        }
        XC_LOGI("stream: PeerConnection avviata");
        updateMicState();
        return true;
    }

    // One reconnection attempt (worker thread, from tick()).
    // Every wait is interruptible: dieNow/abortForExit must not sit behind
    // sleepMs(800) or a blocked start() while the exit monitor shows main=1.
    void reconnect() {
        if (g_appStopping.load() || !running) {
            reconnecting = false;
            return;
        }
        if (reconnectAttempts == 0) platform::notify(ui::tr(ui::Str::Reconnecting));
        if (++reconnectAttempts > 20) {
            end("the connection was lost and couldn't be restored");
            reconnecting = false;
            return;
        }
        xcloud::SessionStatus status;
        std::string err;
        if (!gssv.sessionState(status, err)) {
            XC_LOGW("reconnect %d: no session state (%s)", reconnectAttempts, err.c_str());
            if (g_appStopping.load() || !running) reconnecting = false;
            return;
        }
        if (g_appStopping.load() || !running) {
            reconnecting = false;
            return;
        }
        if (status.state == xcloud::SessionState::Provisioning ||
            status.state == xcloud::SessionState::WaitingForResources) {
            XC_LOGI("reconnect %d: session %s, waiting", reconnectAttempts, status.raw.c_str());
            if (gssv.isHome() && reconnectAttempts >= 4) end("the connection was lost and couldn't be restored");
            return;
        }
        if (status.state != xcloud::SessionState::Provisioned && status.state != xcloud::SessionState::ReadyToConnect) {
            end("the session ended on the server while reconnecting (" + status.raw + ")");
            reconnecting = false;
            return;
        }
        std::shared_ptr<stream::StreamSession> old;
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            old.swap(session);
        }
        if (old) {
            // halt subito, close sul thread staccato: il worker non si
            // blocca e i keepalive STUN del peer vecchio finiscono.
            try {
                old->releaseAgent();
            } catch (...) {
            }
            XC_LOGI("webrtc: peer precedente chiuso");
        }
        if (g_appStopping.load() || !running) {
            reconnecting = false;
            return;
        }
        // Stesse opzioni della sessione originale (bitrate/risoluzione pieni):
        // niente ripartenza a 2 kbps per colpa di opts azzerate.
        stream::StreamOptions opts = options;
        opts.connectTimeoutMs = kReconnectDtlsBudgetMs;
        auto next = std::make_shared<stream::StreamSession>(gssv, callbacks, opts);
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            session = next;
        }
        if (!g_appStopping.load()) stream::armNewSession();
        if (!next->start(err)) {
            XC_LOGW("reconnect %d failed: %s", reconnectAttempts, err.c_str());
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                if (session == next) session.reset();
            }
            try {
                next->releaseAgent();
            } catch (...) {
            }
            if (g_appStopping.load() || !running) reconnecting = false;
            else reconnecting = true;
            return;
        }
        if (g_appStopping.load() || !running) {
            try {
                next->abandon();
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                if (session == next) session.reset();
            }
            reconnecting = false;
            return;
        }
        reconnecting = false;
        tierSwapArmed = 0;
        updateMicState();
        XC_LOGI("reconnected after %d attempt(s)", reconnectAttempts);
        reconnectAttempts = 0;
        silentTicks = 0;
        lastRtpPackets = 0;
        lastAudioPackets = 0;
        ++reconnects;
        needDecoderReset.store(true);
        keyframeWanted = true;
        platform::notify(ui::tr(ui::Str::Reconnected));
    }

    // Returns true when a snapshot was taken: the caller then saves what the
    // screen shows too (saveScreen()).
    bool saveSnapshotIfAsked(const media::Picture& pic) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            path.swap(snapshotPath);
        }
        if (path.empty()) return false;
        screenPath = path.substr(0, path.rfind('/') + 1) + "screen.ppm";
        int w = pic.width / 2, h = pic.height / 2;
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int c = (pic.y[(2 * y) * pic.strideY + 2 * x] - 16) * 1192;
                int d = pic.u[y * pic.strideU + x] - 128;
                int e = pic.v[y * pic.strideV + x] - 128;
                auto clamp = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
                uint8_t* o = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                o[0] = clamp((c + 1836 * e + 512) >> 10);
                o[1] = clamp((c - 218 * d - 546 * e + 512) >> 10);
                o[2] = clamp((c + 2163 * d + 512) >> 10);
            }
        std::string data = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
        data.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        bool ok = platform::writeFileAtomic(path, data);
        XC_LOGI("snapshot %dx%d -> %s: %s", w, h, path.c_str(), ok ? "ok" : "failed");
        return true;
    }

    // The scan-out buffer just presented, at full resolution.
    void saveScreen() {
        std::vector<uint8_t> rgb;
        int w = 0, h = 0;
        if (!display::readBackRgb(rgb, w, h)) return;
        std::string data = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
        data.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        bool ok = platform::writeFileAtomic(screenPath, data);
        XC_LOGI("screen -> %s: %s", screenPath.c_str(), ok ? "ok" : "failed");
    }

    // Video/worker: il limiter (500 ms / 10 in 5 s) vive in StreamSession.
    void requestKeyframeIfWanted() {
        if (!keyframeWanted) return;
        auto s = current();
        if (!s) return;
        if (!s->requestKeyframe()) {
            stats::statsNoteKf(0);
            return;
        }
        keyframeWanted = false;
        lastKeyframeRequestMs = platform::nowMs();
        ++keyframeRequests;
        stats::statsNoteKf(1);
    }

    void pumpFast() {
        if (!running || reconnecting) return;
        if (auto s = current()) s->pumpFast();
    }

    // About once a second (worker thread).
    void tick() {
        if (!running) return;
        if (reconnecting) return reconnect();
        auto s = current();
        if (!s) return;
        s->tick();
        // recv: 1 Hz è sul thread dedicato (psbox_rd_start_logger), non qui:
        // in 01.55 start() bloccava il worker e i contatori non comparivano.
        const auto& vst = s->videoStats();
        stats::statsNoteNet(vst.lost.load(), vst.nacks.load(), vst.recovered.load());
        stats::statsNoteBitrate(vst.estimate.load() ? vst.estimate.load() : options.maxBitrate);
        stats::statsNoteAudio(media::audioUnderruns(), media::audioTrimmedFrames());
        // Video fermo < 15 s: resto in sessione, PLI rate-limited. Niente
        // «Connessione persa». Riconnetto solo se ICE/DTLS è giù, oppure
        // input chiuso AND nessun video > 15 s AND ICE non connesso.
        uint64_t packets = s->videoStats().packets;
        uint64_t audio = audioPackets;
        bool audioAlive = audio != lastAudioPackets;
        lastAudioPackets = audio;
        silentTicks = packets == lastRtpPackets ? silentTicks + 1 : 0;
        lastRtpPackets = packets;
        if (silentTicks >= 1 && silentTicks <= 15 && s->isOpen()) {
            if (silentTicks == 1 || silentTicks % 2 == 0) {
                XC_LOGW("video fermo da %d s%s: resto in sessione, keyframe", silentTicks,
                        audioAlive ? " (l'audio arriva)" : "");
                keyframeWanted = true;
                requestKeyframeIfWanted();
            }
        }
        bool iceOk = s->iceConnected();
        bool inputDead = s->inputChannelDown();
        if (inputDead && silentTicks > 15 && !iceOk && !reconnecting.exchange(true)) {
            XC_LOGW("input chiuso, video fermo da %d s, ICE giù: riconnetto", silentTicks);
        } else if (!s->isOpen() && !iceOk && silentTicks > 15 && !reconnecting.exchange(true)) {
            XC_LOGW("sessione chiusa e ICE giù dopo %d s senza video: riconnetto", silentTicks);
        }

        if (!tierSwapArmed && s->abrDemoteWanted() && preferredTier >= ABR_1080) {
            tierSwapArmed = 1;
            s->clearAbrTierHints();
            XC_LOGW("abr: stall ripetuti a 1080, cedo a 720p @ 8 Mb/s");
            gssv.setResolution(xcloud::Resolution::P720);
            options.resolutionAlias = "720HQ";
            options.maxBitrate = abrCeiling(ABR_720);
            options.abrTier = ABR_720;
            preferredTier = ABR_720;
            wantPromote1080 = 1;
            reconnecting.store(true);
        } else if (!tierSwapArmed && s->abrPromoteWanted() && wantPromote1080) {
            tierSwapArmed = 1;
            s->clearAbrTierHints();
            XC_LOGI("abr: 60 s puliti a 720, offro 1080 @ 10 Mb/s");
            gssv.setResolution(xcloud::Resolution::P1080HQ);
            options.resolutionAlias = "1080HQ";
            options.maxBitrate = abrCeiling(ABR_1080);
            options.abrTier = ABR_1080;
            preferredTier = ABR_1080;
            wantPromote1080 = 0;
            reconnecting.store(true);
        }
    }

    void stop() {
        end("stopped");
        if (auto s = current()) s->close();
        if (videoThread.joinable()) videoThread.join();
        if (audioThread.joinable()) audioThread.join();
        std::shared_ptr<stream::StreamSession> last;
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            last = std::move(session);
        }
        last.reset();  // its teardown waits for libdatachannel's callbacks: not under the lock
        closeMic();
#if defined(XCLOUD_PS4)
        if (orbisAudio && audio_callbacks_orbis.cleanup) {
            audio_callbacks_orbis.cleanup();
            orbisAudio = false;
        }
#endif
        media::audioStop();
        for (int i = 0; i < input::kMaxPads; ++i) {
            input::setRumble(0, 0, 0, i);
            input::setTriggerRumble(0, 0, 0, i);
        }
    }

    // Come stop(), ma senza join / pc->close / resetCallbacks: in chiusura
    // di sistema quelle chiamate restano appese (thread in Decode/Output o
    // callback RTP) → CE-34878-0. Solo flag + null guard; _exit ripulisce.
    void abortForExit() {
        xc_log_stamp("stream: abort enter");
        running = false;
        reconnecting = false;
        end("exit");
        cv.notify_all();
        audioCv.notify_all();
        xc_log_stamp("stream: requestStop");
        stream::requestStop();
        xc_log_stamp("stream: audioAbort");
        media::audioAbortForExit();
        xc_log_stamp("stream: videoRelease");
        display::releaseForExit();
        xc_log_stamp("stream: session abandon (no resetCallbacks)");
        std::shared_ptr<stream::StreamSession> last;
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            last = std::move(session);
        }
        if (last) {
            try {
                last->abandon();
            } catch (...) {
            }
            // Non distruggere: il dtor di PeerConnection può appendersi.
            static std::vector<std::shared_ptr<stream::StreamSession>> abandoned;
            abandoned.push_back(std::move(last));
        }
        for (int i = 0; i < input::kMaxPads; ++i) {
            input::setRumble(0, 0, 0, i);
            input::setTriggerRumble(0, 0, 0, i);
        }
        xc_log_stamp("stream: abort leave");
    }
};

StreamPlayer::StreamPlayer(xcloud::GssvClient& gssv) : impl_(std::make_unique<Impl>(gssv)) {}
StreamPlayer::~StreamPlayer() { impl_->stop(); }

bool StreamPlayer::start(std::string& err) { return impl_->start(err); }
bool StreamPlayer::running() const { return impl_->running; }

std::string StreamPlayer::endReason() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->endReason;
}

void StreamPlayer::sendInput(const input::ControllerState& p, int index) {
    auto s = impl_->current();
    if (!s || !impl_->running) return;
    static int once = 0;
    if (!once++) XC_LOGI("input: primo pad");
    stream::GamepadFrame f;
    f.index = static_cast<uint8_t>(index);
#if defined(XCLOUD_PS4)
    // Menu PS / chiusura: niente send sul DataChannel da qui (può bloccare
    // main/monitor). armNeutralPad + pumpFast mandano il neutro.
    if (g_appStopping.load()) return;
    if (g_psMenu.load()) {
        s->armNeutralPad();
        return;
    }
#endif
    auto set = [&](bool on, uint16_t bit) {
        if (on) f.buttons |= bit;
    };
    set(p.btnA, stream::kA);
    set(p.btnB, stream::kB);
    set(p.btnX, stream::kX);
    set(p.btnY, stream::kY);
    set(p.dpadUp, stream::kDPadUp);
    set(p.dpadDown, stream::kDPadDown);
    set(p.dpadLeft, stream::kDPadLeft);
    set(p.dpadRight, stream::kDPadRight);
    set(p.btnL1, stream::kLeftShoulder);
    set(p.btnR1, stream::kRightShoulder);
    set(p.btnL3, stream::kLeftThumb);
    set(p.btnR3, stream::kRightThumb);
    set(p.btnOptions, stream::kMenu);
    set(p.btnTouchpad, stream::kView);
    set(p.btnNexus, stream::kNexus);
    f.leftX = p.leftStickX;
    f.leftY = -p.leftStickY;  // pad: +down; wire: +up
    f.rightX = p.rightStickX;
    f.rightY = -p.rightStickY;
    f.leftTrigger = p.triggerL2;
    f.rightTrigger = p.triggerR2;
    s->sendGamepad(f);
}

bool StreamPlayer::titleFocused() const { return impl_->titleFocused; }

void StreamPlayer::setTouchEnabled(bool on) {
    if (auto s = impl_->current(); s && impl_->running) s->setTouchEnabled(on);
}

void StreamPlayer::setPadConnected(int index, bool connected) {
    if (auto s = impl_->current(); s && impl_->running) s->setGamepadConnected(index, connected);
}

bool StreamPlayer::takeTextInput(stream::TextInputRequest& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    while (!impl_->textRequests.empty()) {
        out = std::move(impl_->textRequests.front());
        impl_->textRequests.pop_front();
        if (!impl_->textWithdrawn.count(out.id)) return true;
    }
    return false;
}

bool StreamPlayer::textInputWithdrawn(const std::string& id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->textWithdrawn.count(id) != 0;
}

void StreamPlayer::answerTextInput(const std::string& id, bool accepted, const std::string& text) {
    auto s = impl_->current();
    if (textInputWithdrawn(id) || !s) return;
    XC_LOGI("text input %s", accepted ? "sent" : "cancelled");
    if (accepted)
        s->completeTextInput(id, text);
    else
        s->cancelTextInput(id);
}

void StreamPlayer::requestKeyframe() { impl_->keyframeWanted = true; }
void StreamPlayer::armReferenceLoss() { impl_->armReferenceLoss(); }

uint32_t StreamPlayer::reconnects() const { return impl_->reconnects; }

void StreamPlayer::simulateDrop() {
    if (auto s = impl_->current()) s->simulateDrop();
}

void StreamPlayer::requestResolution(const std::string& alias) {
    if (auto s = impl_->current()) s->requestResolution(alias);
}

void StreamPlayer::pumpFast() { impl_->pumpFast(); }
void StreamPlayer::tick() { impl_->tick(); }

void StreamPlayer::dumpVideo(const std::string& path, int seconds) {
    // Before start(): only the video thread touches the file afterwards.
    impl_->dumpFile = std::fopen(path.c_str(), "wb");
    impl_->dumpUntilMs = platform::nowMs() + static_cast<uint64_t>(seconds) * 1000u;
    XC_LOGI("dumping video to %s: %s", path.c_str(), impl_->dumpFile ? "ok" : "failed");
}

void StreamPlayer::requestSnapshot(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    impl_->snapshotPath = path;
}
void StreamPlayer::stop() { impl_->stop(); }
void StreamPlayer::abortForExit() { impl_->abortForExit(); }

void StreamPlayer::setHwDecodeProbe(bool on) { impl_->hwProbe = on; }

void StreamPlayer::setDecodeThreads(int threads) { impl_->decodeThreads = std::max(1, threads); }
void StreamPlayer::setMaxBitrate(int mbps) { impl_->maxBitrateMbps = std::max(0, mbps); }
void StreamPlayer::setSimulatedLoss(int percent) { impl_->simulatedLoss = std::clamp(percent, 0, 50); }

StreamPlayer::Stats StreamPlayer::stats() const {
    Stats st;
    st.videoFrames = impl_->videoFrames;
    st.decodedFrames = impl_->decodedFrames;
    st.droppedFrames = impl_->droppedFrames;
    st.audioPackets = impl_->audioPackets;
    st.decodeFailures = impl_->decodeFailures;
    st.queueResets = impl_->queueResets;
    st.keyframeRequests = impl_->keyframeRequests;
    st.vibrations = impl_->vibrations;
    st.lateFrames = impl_->lateFrames;
    st.width = static_cast<int>(impl_->pictureSize >> 16);
    st.height = static_cast<int>(impl_->pictureSize & 0xFFFF);
    // Timings since the previous call.
    uint64_t dc = impl_->decodeCalls.exchange(0), du = impl_->decodeUs.exchange(0);
    uint64_t rc = impl_->drawCalls.exchange(0), ru = impl_->drawUs.exchange(0);
    st.decodeAvgUs = dc ? du / dc : 0;
    st.decodeMaxUs = impl_->decodeMaxUs.exchange(0);
    st.drawAvgUs = rc ? ru / rc : 0;
    st.drawMaxUs = impl_->drawMaxUs.exchange(0);
    uint64_t dn = impl_->displayCount.exchange(0), du2 = impl_->displayUs.exchange(0);
    st.displayAvgUs = dn ? du2 / dn : 0;
    st.displayMaxUs = impl_->displayMaxUs.exchange(0);
    if (auto s = impl_->current()) {
        const auto& v = s->videoStats();
        st.rtpPackets = v.packets;
        st.rtpLost = v.lost;
        st.rtpRecovered = v.recovered;
        st.rtpNacks = v.nacks;
        st.rtpDroppedFrames = v.framesDropped;
        st.keyframeRequests += v.keyframeRequests;
        st.rtpKbps = v.receiveRate / 1000;
        st.rttMs = s->rttMs();
        st.rembKbps = v.estimate / 1000;
        st.rtcpTx = v.rtcpReports;
        st.twccTx = v.twccSent;
        st.acks = s->framesAcked();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        st.queued = impl_->frameQ.sizeApprox();
    }
    return st;
}

void StreamPlayer::setMicEnabled(bool enabled) {
    impl_->micEnabled = enabled;
    impl_->updateMicState();
}

bool StreamPlayer::micEnabled() const {
    return impl_->micEnabled;
}

void StreamPlayer::setMicMuted(bool muted) {
    impl_->micMuted = muted;
    impl_->audioIn.setMuted(muted);
}

bool StreamPlayer::micMuted() const {
    return impl_->micMuted;
}

bool StreamPlayer::isMicHardwareMuted() const {
    return impl_ ? impl_->audioIn.isHardwareMuted() : false;
}

void StreamPlayer::setMicGain(float gain) {
    impl_->micGain = gain;
    impl_->audioIn.setGain(gain);
}

float StreamPlayer::micGain() const {
    return impl_->micGain;
}

float StreamPlayer::micLevel() const {
    return impl_ ? impl_->audioIn.level() : 0.0f;
}

}  // namespace xc::app
