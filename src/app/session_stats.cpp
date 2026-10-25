// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "app/session_stats.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace xc::app::stats {

namespace {

std::mutex g_writeMu;
std::atomic<int> g_on{0};
std::atomic<int> g_stop{0};
std::string g_dir;
std::string g_path;
std::string g_started;
std::string g_resolution;
std::atomic<uint32_t> g_ask{0};
std::atomic<uint32_t> g_nowBps{0};
std::atomic<uint64_t> g_t0ms{0};
std::atomic<int> g_interval{kStatsIntervalMs};

std::atomic<uint64_t> g_recv{0}, g_decN{0}, g_pres{0};
std::atomic<uint64_t> g_dropQ{0}, g_dropI{0}, g_dropE{0}, g_dropL{0};
std::atomic<uint64_t> g_lost{0}, g_nack{0}, g_recov{0};
std::atomic<uint64_t> g_kfSent{0}, g_kfBlock{0};
std::atomic<uint64_t> g_qSum{0}, g_qN{0};
std::atomic<uint32_t> g_qMax{0};
std::atomic<uint64_t> g_decSum{0}, g_presSum{0};
std::atomic<uint64_t> g_decBucket[kTimeBuckets];
std::atomic<uint64_t> g_presBucket[kTimeBuckets];
std::atomic<uint64_t> g_underrun{0}, g_trim{0};
std::atomic<uint64_t> g_sus{0}, g_res{0};

struct BrSlot {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> tMs{0};
    std::atomic<uint32_t> bps{0};
};
BrSlot g_br[24];
std::atomic<uint32_t> g_brSeq{0};

char g_exit[48] = "in corso";
std::atomic<int> g_exitSet{0};
std::atomic<int> g_gen{0};
std::atomic<int> g_alive{0};

uint64_t monoMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

int bucketOf(uint64_t us) {
    static const uint64_t kEdge[kTimeBuckets - 1] = {1000, 2000, 4000, 8000, 12000, 16000,
                                                      24000, 33000, 50000, 80000, 120000};
    for (int i = 0; i < kTimeBuckets - 1; ++i)
        if (us < kEdge[i]) return i;
    return kTimeBuckets - 1;
}

void addHist(std::atomic<uint64_t>* buckets, std::atomic<uint64_t>& sum, std::atomic<uint64_t>& n, uint64_t us) {
    buckets[bucketOf(us)].fetch_add(1, std::memory_order_relaxed);
    sum.fetch_add(us, std::memory_order_relaxed);
    n.fetch_add(1, std::memory_order_relaxed);
}

TimeHist loadHist(const std::atomic<uint64_t>* buckets, uint64_t sum, uint64_t n) {
    TimeHist h;
    h.n = n;
    h.sumUs = sum;
    for (int i = 0; i < kTimeBuckets; ++i) h.bucket[i] = buckets[i].load(std::memory_order_relaxed);
    return h;
}

void lowPrio() {
    sched_param sp{};
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
#ifdef SCHED_IDLE
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
#endif
    (void)setpriority(PRIO_PROCESS, 0, 10);
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        if (c == '\n' || c == '\r') continue;
        o.push_back(c);
    }
    return o;
}

SessionSnap takeSnap() {
    SessionSnap s;
    s.started = g_started;
    uint64_t t0 = g_t0ms.load();
    uint64_t now = monoMs();
    s.durationMs = t0 && now > t0 ? now - t0 : 0;
    s.resolution = g_resolution;
    s.bitrateAskBps = g_ask.load();
    s.bitrateNowBps = g_nowBps.load();
    uint32_t seq = g_brSeq.load();
    int n = seq > 24 ? 24 : static_cast<int>(seq);
    int base = seq > 24 ? static_cast<int>(seq - 24) : 0;
    s.bitrateN = 0;
    for (int i = 0; i < n; ++i) {
        int slot = (base + i) % 24;
        uint32_t published = g_br[slot].seq.load();
        if (published == 0) continue;
        s.bitrate[s.bitrateN].tMs = g_br[slot].tMs.load();
        s.bitrate[s.bitrateN].bps = g_br[slot].bps.load();
        if (g_br[slot].seq.load() != published) continue;
        ++s.bitrateN;
    }
    s.framesReceived = g_recv.load();
    s.framesDecoded = g_decN.load();
    s.framesPresented = g_pres.load();
    s.droppedQueue = g_dropQ.load();
    s.droppedIdr = g_dropI.load();
    s.droppedError = g_dropE.load();
    s.droppedLatest = g_dropL.load();
    double sec = s.durationMs > 0 ? s.durationMs / 1000.0 : 0;
    if (sec > 0) {
        s.fpsReceived = s.framesReceived / sec;
        s.fpsDecoded = s.framesDecoded / sec;
        s.fpsPresented = s.framesPresented / sec;
    }
    s.packetsLost = g_lost.load();
    s.nacksSent = g_nack.load();
    s.nacksRecovered = g_recov.load();
    s.keyframeSent = g_kfSent.load();
    s.keyframeBlocked = g_kfBlock.load();
    uint64_t qn = g_qN.load();
    s.queueMax = g_qMax.load();
    s.queueAvg = qn ? static_cast<double>(g_qSum.load()) / static_cast<double>(qn) : 0;
    TimeHist dec = loadHist(g_decBucket, g_decSum.load(), g_decN.load());
    TimeHist pres = loadHist(g_presBucket, g_presSum.load(), g_pres.load());
    s.decodeMeanUs = timeHistMean(dec);
    s.decodeP95Us = timeHistP95(dec);
    s.presentMeanUs = timeHistMean(pres);
    s.presentP95Us = timeHistP95(pres);
    s.audioUnderrun = g_underrun.load();
    s.audioTrimFrames = g_trim.load();
    s.menuSuspend = g_sus.load();
    s.menuResume = g_res.load();
    s.exitReason = g_exit;
    return s;
}

void writeFile(const SessionSnap& snap) {
    if (g_path.empty()) return;
    std::string body = renderSessionJson(snap);
    std::string tmp = g_path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || std::rename(tmp.c_str(), g_path.c_str()) != 0) std::remove(tmp.c_str());
}

void pruneDir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 12 && n.compare(0, 7, "stream-") == 0 && n.size() >= 5 &&
            n.compare(n.size() - 5, 5, ".json") == 0)
            names.push_back(n);
    }
    closedir(d);
    if (names.size() <= static_cast<size_t>(kStatsKeepFiles)) return;
    std::sort(names.begin(), names.end());
    int drop = statsPruneCount(static_cast<int>(names.size()), kStatsKeepFiles);
    for (int i = 0; i < drop; ++i) std::remove((dir + "/" + names[static_cast<size_t>(i)]).c_str());
}

void loop(int gen) {
    lowPrio();
    g_alive.fetch_add(1);
    while (g_gen.load() == gen && !g_stop.load()) {
        int slice = g_interval.load();
        if (slice < 100) slice = 100;
        for (int left = slice; left > 0 && g_gen.load() == gen && !g_stop.load(); left -= 100) usleep(100 * 1000);
        if (g_gen.load() != gen) break;
        if (!g_writeMu.try_lock()) continue;
        writeFile(takeSnap());
        g_writeMu.unlock();
    }
    if (g_gen.load() == gen && g_writeMu.try_lock()) {
        writeFile(takeSnap());
        g_writeMu.unlock();
    }
    g_alive.fetch_sub(1);
}

}  // namespace

uint64_t timeBucketEdge(int i) {
    static const uint64_t kEdge[kTimeBuckets] = {1000, 2000, 4000, 8000, 12000, 16000,
                                                  24000, 33000, 50000, 80000, 120000, 0};
    if (i < 0 || i >= kTimeBuckets) return 0;
    return kEdge[i];
}

void timeHistAdd(TimeHist& h, uint64_t us) {
    int i = 0;
    for (; i < kTimeBuckets - 1; ++i)
        if (us < timeBucketEdge(i)) break;
    h.bucket[i]++;
    h.sumUs += us;
    h.n++;
}

uint64_t timeHistMean(const TimeHist& h) { return h.n ? h.sumUs / h.n : 0; }

uint64_t timeHistP95(const TimeHist& h) {
    if (!h.n) return 0;
    uint64_t need = (h.n * 95 + 99) / 100;
    if (need < 1) need = 1;
    uint64_t acc = 0;
    for (int i = 0; i < kTimeBuckets; ++i) {
        acc += h.bucket[i];
        if (acc >= need) return timeBucketEdge(i) ? timeBucketEdge(i) : h.sumUs / h.n;
    }
    return timeBucketEdge(kTimeBuckets - 2);
}

std::string sessionFileName(const char* ymd, const char* hms) {
    std::string s = "stream-";
    if (ymd) s += ymd;
    s += "-";
    if (hms) s += hms;
    s += ".json";
    return s;
}

int statsPruneCount(int have, int keep) {
    if (keep < 1) keep = 1;
    if (have <= keep) return 0;
    return have - keep;
}

std::string renderSessionJson(const SessionSnap& s) {
    char buf[256];
    std::string o;
    o.reserve(2048);
    o += "{\n";
    o += "  \"v\": 1,\n";
    o += "  \"started\": \"" + jsonEscape(s.started) + "\",\n";
    std::snprintf(buf, sizeof buf, "  \"duration_s\": %.3f,\n", s.durationMs / 1000.0);
    o += buf;
    o += "  \"resolution\": \"" + jsonEscape(s.resolution) + "\",\n";
    std::snprintf(buf, sizeof buf, "  \"bitrate_ask_bps\": %u,\n  \"bitrate_now_bps\": %u,\n", s.bitrateAskBps,
                  s.bitrateNowBps);
    o += buf;
    o += "  \"bitrate_changes\": [";
    for (int i = 0; i < s.bitrateN; ++i) {
        std::snprintf(buf, sizeof buf, "%s{\"t_s\": %.3f, \"bps\": %u}", i ? ", " : "", s.bitrate[i].tMs / 1000.0,
                      s.bitrate[i].bps);
        o += buf;
    }
    o += "],\n";
    std::snprintf(buf, sizeof buf, "  \"fps\": {\"received\": %.2f, \"decoded\": %.2f, \"presented\": %.2f},\n",
                  s.fpsReceived, s.fpsDecoded, s.fpsPresented);
    o += buf;
    std::snprintf(buf, sizeof buf,
                  "  \"frames\": {\"received\": %llu, \"decoded\": %llu, \"presented\": %llu, "
                  "\"dropped_queue\": %llu, \"dropped_idr\": %llu, \"dropped_error\": %llu, "
                  "\"dropped_latest\": %llu},\n",
                  (unsigned long long)s.framesReceived, (unsigned long long)s.framesDecoded,
                  (unsigned long long)s.framesPresented, (unsigned long long)s.droppedQueue,
                  (unsigned long long)s.droppedIdr, (unsigned long long)s.droppedError,
                  (unsigned long long)s.droppedLatest);
    o += buf;
    std::snprintf(buf, sizeof buf,
                  "  \"net\": {\"lost\": %llu, \"nack_sent\": %llu, \"nack_recovered\": %llu},\n",
                  (unsigned long long)s.packetsLost, (unsigned long long)s.nacksSent,
                  (unsigned long long)s.nacksRecovered);
    o += buf;
    std::snprintf(buf, sizeof buf, "  \"keyframe\": {\"sent\": %llu, \"blocked\": %llu},\n",
                  (unsigned long long)s.keyframeSent, (unsigned long long)s.keyframeBlocked);
    o += buf;
    std::snprintf(buf, sizeof buf, "  \"queue\": {\"max\": %u, \"avg\": %.2f},\n", s.queueMax, s.queueAvg);
    o += buf;
    std::snprintf(buf, sizeof buf, "  \"decode_us\": {\"mean\": %llu, \"p95\": %llu},\n",
                  (unsigned long long)s.decodeMeanUs, (unsigned long long)s.decodeP95Us);
    o += buf;
    std::snprintf(buf, sizeof buf, "  \"present_us\": {\"mean\": %llu, \"p95\": %llu},\n",
                  (unsigned long long)s.presentMeanUs, (unsigned long long)s.presentP95Us);
    o += buf;
    std::snprintf(buf, sizeof buf, "  \"audio\": {\"underrun\": %llu, \"trim_frames\": %llu},\n",
                  (unsigned long long)s.audioUnderrun, (unsigned long long)s.audioTrimFrames);
    o += buf;
    std::snprintf(buf, sizeof buf, "  \"menu\": {\"suspend\": %llu, \"resume\": %llu},\n",
                  (unsigned long long)s.menuSuspend, (unsigned long long)s.menuResume);
    o += buf;
    o += "  \"exit\": \"" + jsonEscape(s.exitReason) + "\"\n";
    o += "}\n";
    return o;
}

void statsBegin(const std::string& dir, const char* resolution, uint32_t askBps, int intervalMs) {
    g_stop.store(1);
    g_gen.fetch_add(1);
    for (int i = 0; i < 50 && g_alive.load(); ++i) usleep(10 * 1000);
    g_stop.store(0);
    g_dir = dir;
    ::mkdir(dir.c_str(), 0777);
    pruneDir(dir);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char ymd[32], hms[32], stamp[64];
    std::snprintf(ymd, sizeof ymd, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    std::snprintf(hms, sizeof hms, "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    g_started = stamp;
    std::string name = sessionFileName(ymd, hms);
    std::string path = dir + "/" + name;
    if (::access(path.c_str(), F_OK) == 0) {
        char alt[32];
        std::snprintf(alt, sizeof alt, "-%u", g_brSeq.load() + 1);
        name = std::string("stream-") + ymd + "-" + hms + alt + ".json";
        path = dir + "/" + name;
    }
    g_path = path;
    g_resolution = resolution ? resolution : "";
    g_ask.store(askBps);
    g_nowBps.store(askBps);
    g_t0ms.store(monoMs());
    g_interval.store(intervalMs > 0 ? intervalMs : kStatsIntervalMs);
    g_recv = g_decN = g_pres = 0;
    g_dropQ = g_dropI = g_dropE = g_dropL = 0;
    g_lost = g_nack = g_recov = 0;
    g_kfSent = g_kfBlock = 0;
    g_qSum = g_qN = 0;
    g_qMax = 0;
    g_decSum = g_presSum = 0;
    for (int i = 0; i < kTimeBuckets; ++i) {
        g_decBucket[i].store(0);
        g_presBucket[i].store(0);
    }
    g_underrun = g_trim = g_sus = g_res = 0;
    g_brSeq.store(0);
    for (auto& b : g_br) {
        b.seq.store(0);
        b.tMs.store(0);
        b.bps.store(0);
    }
    std::snprintf(g_exit, sizeof g_exit, "in corso");
    g_exitSet.store(0);
    uint32_t seq = g_brSeq.fetch_add(1) + 1;
    g_br[0].tMs.store(0);
    g_br[0].bps.store(askBps);
    g_br[0].seq.store(seq);
    g_on.store(1);
    int gen = g_gen.load();
    std::thread(loop, gen).detach();
}

void statsNoteQueue(uint32_t depth) {
    if (!g_on.load()) return;
    g_qN.fetch_add(1, std::memory_order_relaxed);
    g_qSum.fetch_add(depth, std::memory_order_relaxed);
    uint32_t m = g_qMax.load(std::memory_order_relaxed);
    while (depth > m && !g_qMax.compare_exchange_weak(m, depth, std::memory_order_relaxed)) {
    }
}

void statsNoteDrop(int reason, uint64_t n) {
    if (!g_on.load() || !n) return;
    if (reason == 0) g_dropQ.fetch_add(n, std::memory_order_relaxed);
    else if (reason == 1) g_dropI.fetch_add(n, std::memory_order_relaxed);
    else if (reason == 2) g_dropE.fetch_add(n, std::memory_order_relaxed);
    else g_dropL.fetch_add(n, std::memory_order_relaxed);
}

void statsNoteDecoded(uint64_t us) {
    if (!g_on.load()) return;
    addHist(g_decBucket, g_decSum, g_decN, us);
}

void statsNotePresented(uint64_t us) {
    if (!g_on.load()) return;
    addHist(g_presBucket, g_presSum, g_pres, us);
}

void statsNoteKf(int sent) {
    if (!g_on.load()) return;
    if (sent) g_kfSent.fetch_add(1, std::memory_order_relaxed);
    else g_kfBlock.fetch_add(1, std::memory_order_relaxed);
}

void statsNoteNet(uint64_t lost, uint64_t nacks, uint64_t recovered) {
    if (!g_on.load()) return;
    g_lost.store(lost, std::memory_order_relaxed);
    g_nack.store(nacks, std::memory_order_relaxed);
    g_recov.store(recovered, std::memory_order_relaxed);
}

void statsNoteBitrate(uint32_t bps) {
    if (!g_on.load()) return;
    uint32_t prev = g_nowBps.exchange(bps);
    if (prev == bps) return;
    uint32_t n = g_brSeq.fetch_add(1);
    int slot = static_cast<int>(n % 24);
    uint64_t t0 = g_t0ms.load();
    uint64_t now = monoMs();
    g_br[slot].seq.store(0);
    g_br[slot].tMs.store(t0 && now > t0 ? static_cast<uint32_t>(now - t0) : 0);
    g_br[slot].bps.store(bps);
    g_br[slot].seq.store(n + 1);
}

void statsNoteAudio(uint64_t underrun, uint64_t trimFrames) {
    if (!g_on.load()) return;
    g_underrun.store(underrun, std::memory_order_relaxed);
    g_trim.store(trimFrames, std::memory_order_relaxed);
}

void statsNoteMenu(int suspend) {
    if (!g_on.load()) return;
    if (suspend) g_sus.fetch_add(1, std::memory_order_relaxed);
    else g_res.fetch_add(1, std::memory_order_relaxed);
}

void statsNoteReceived(uint64_t n) {
    if (!g_on.load() || !n) return;
    g_recv.fetch_add(n, std::memory_order_relaxed);
}

void statsFinish(const char* exitReason) {
    if (!g_on.load()) return;
    if (exitReason && exitReason[0]) {
        std::snprintf(g_exit, sizeof g_exit, "%s", exitReason);
        g_exitSet.store(1);
    }
    g_stop.store(1);
}

void statsWriteFinal() {
    if (!g_on.load() || g_path.empty()) return;
    if (!g_writeMu.try_lock()) return;
    writeFile(takeSnap());
    g_writeMu.unlock();
}

}  // namespace xc::app::stats
