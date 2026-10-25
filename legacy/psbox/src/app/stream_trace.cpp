// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/stream_trace.h"

#include "platform/platform.h"
#include "util/log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace xc::app::trace {
namespace {

constexpr uint64_t kBudgetUs = 16666;
std::mutex g_mu;
std::string g_dir;
std::atomic<bool> g_on{false};
uint64_t g_t0Ms = 0;
uint64_t g_csvRows = 0;
FILE* g_csv = nullptr;
FILE* g_sum = nullptr;

void writeStageFile(const char* stage) {
    if (g_dir.empty() || !stage) return;
    std::string path = g_dir + "/last_stage.txt";
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char line[192];
    int n = std::snprintf(line, sizeof(line), "%llu %s\n",
                          static_cast<unsigned long long>(platform::nowMs()), stage);
    if (n > 0) {
        (void)!::write(fd, line, static_cast<size_t>(n));
        (void)!::fsync(fd);
    }
    ::close(fd);
}

}  // namespace

void beginSession() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_dir = platform::dataDir();
    g_t0Ms = platform::nowMs();
    g_csvRows = 0;
    if (g_csv) {
        std::fclose(g_csv);
        g_csv = nullptr;
    }
    if (g_sum) {
        std::fclose(g_sum);
        g_sum = nullptr;
    }
    std::string csvPath = g_dir + "/trace_stream.csv";
    std::string sumPath = g_dir + "/trace_stream.txt";
    g_csv = std::fopen(csvPath.c_str(), "w");
    g_sum = std::fopen(sumPath.c_str(), "w");
    if (g_csv) {
        std::fputs("t_ms,frame,src_w,src_h,dst_w,dst_h,scaled,pres_us,budget_us,over,drawn\n", g_csv);
        std::fflush(g_csv);
        ::fsync(fileno(g_csv));
    }
    if (g_sum) {
        std::fprintf(g_sum, "TRACE_BEGIN t0=%llu\n", static_cast<unsigned long long>(g_t0Ms));
        std::fflush(g_sum);
        ::fsync(fileno(g_sum));
    }
    g_on.store(true, std::memory_order_release);
    writeStageFile("SESSION_BEGIN");
    XC_LOGI("trace: %s (csv+last_stage)", csvPath.c_str());
}

void endSession() {
    setStage("SESSION_END");
    std::lock_guard<std::mutex> lock(g_mu);
    g_on.store(false, std::memory_order_release);
    if (g_sum) {
        std::fprintf(g_sum, "TRACE_END rows=%llu\n", static_cast<unsigned long long>(g_csvRows));
        std::fflush(g_sum);
        ::fsync(fileno(g_sum));
        std::fclose(g_sum);
        g_sum = nullptr;
    }
    if (g_csv) {
        std::fflush(g_csv);
        ::fsync(fileno(g_csv));
        std::fclose(g_csv);
        g_csv = nullptr;
    }
}

void setStage(const char* stage) {
    if (!stage) return;
    // Sempre su disco: anche se beginSession non e' ancora partito.
    if (g_dir.empty()) g_dir = platform::dataDir();
    writeStageFile(stage);
    xc_log_stamp(stage);
}

void notePresent(uint64_t frame, int srcW, int srcH, int dstW, int dstH, uint64_t presentUs, bool drawn) {
    if (!g_on.load(std::memory_order_acquire)) return;
    const int scaled = (srcW != dstW || srcH != dstH) ? 1 : 0;
    const int over = (drawn && presentUs > kBudgetUs) ? 1 : 0;
    const uint64_t tMs = platform::nowMs() - g_t0Ms;
    bool writeRow = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ++g_csvRows;
        // Primi 120 frame, poi 1/30, sempre se over o draw fallito.
        writeRow = g_csvRows <= 120 || (g_csvRows % 30) == 0 || over || !drawn;
        if (writeRow && g_csv) {
            std::fprintf(g_csv, "%llu,%llu,%d,%d,%d,%d,%d,%llu,%llu,%d,%d\n",
                         static_cast<unsigned long long>(tMs), static_cast<unsigned long long>(frame), srcW, srcH,
                         dstW, dstH, scaled, static_cast<unsigned long long>(presentUs),
                         static_cast<unsigned long long>(kBudgetUs), over, drawn ? 1 : 0);
            std::fflush(g_csv);
            if (over || !drawn || g_csvRows <= 5 || (g_csvRows % 120) == 0) ::fsync(fileno(g_csv));
        }
    }
    if (over && (frame % 60) == 0) {
        XC_LOGW("trace: present over budget %llu us (src %dx%d -> %dx%d)",
                static_cast<unsigned long long>(presentUs), srcW, srcH, dstW, dstH);
    }
}

void tickSecond(uint64_t presented, uint64_t late, uint64_t decodeFail, uint64_t rtpPkts) {
    if (!g_on.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_sum) return;
    uint64_t tMs = platform::nowMs() - g_t0Ms;
    std::fprintf(g_sum, "t=%llu presented=%llu late=%llu dec_fail=%llu rtp=%llu csv_rows=%llu\n",
                 static_cast<unsigned long long>(tMs), static_cast<unsigned long long>(presented),
                 static_cast<unsigned long long>(late), static_cast<unsigned long long>(decodeFail),
                 static_cast<unsigned long long>(rtpPkts), static_cast<unsigned long long>(g_csvRows));
    std::fflush(g_sum);
    ::fsync(fileno(g_sum));
}

}  // namespace xc::app::trace
