// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "util/log.h"

#include "app/crash_guard.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

namespace xc::log {

namespace {
std::mutex g_mutex;
std::atomic<Level> g_level{Level::Info};
FILE* g_file = nullptr;
int g_ready = 0;

const char* tag(Level l) {
    switch (l) {
        case Level::Debug: return "D";
        case Level::Info: return "I";
        case Level::Warn: return "W";
        case Level::Error: return "E";
    }
    return "?";
}
}  // namespace

void setLevel(Level level) { g_level = level; }

void setFile(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = path ? std::fopen(path, "a") : nullptr;
    if (g_file) std::setvbuf(g_file, nullptr, _IONBF, 0);
    g_ready = g_file != nullptr;
    xc::app::crash::setLogFd(g_file ? fileno(g_file) : -1);
}

extern "C" void xc_log_stamp(const char* msg) {
    if (!msg || !msg[0] || !g_file) return;
    char line[180];
    int n = std::snprintf(line, sizeof line, "[I] %s\n", msg);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    int fd = fileno(g_file);
    (void)!::write(fd, line, (size_t)n);
    (void)!::fsync(fd);
}

void write(Level level, const char* fmt, ...) {
    if (level < g_level) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);  // std::localtime's result is shared by every thread
    char ts[32];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &local);

    char line[2200];
    int n = std::snprintf(line, sizeof line, "%s [%s] %s\n", ts, tag(level), msg);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof line)) {
        n = static_cast<int>(sizeof line) - 1;
        line[n - 1] = '\n';
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        (void)!::write(STDERR_FILENO, line, static_cast<size_t>(n));
        if (g_file) {
            (void)!::write(fileno(g_file), line, static_cast<size_t>(n));
            (void)!::fsync(fileno(g_file));
        }
    }
    // Il fsync è già tornato. Per le righe "init …" un timbro senza frame
    // dice se si muore nell'unlock o in noteProgress.
    bool initLine = std::strncmp(msg, "init ", 5) == 0;
    if (initLine) xc_log_stamp("init after fsync");
    // Il marker sta fuori dal mutex: un fault qui non tiene g_mutex.
    xc::app::crash::noteProgress(msg);
    if (initLine) xc_log_stamp("init after note");
}

void raw(const char* msg) {
    if (!msg) return;
    char line[180];
    int n = std::snprintf(line, sizeof line, "[I] %s\n", msg);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_file) return;
        (void)!::write(fileno(g_file), line, (size_t)n);
        (void)!fsync(fileno(g_file));
    }
    xc::app::crash::noteProgress(msg);
}

int fileReady() { return g_ready; }

}  // namespace xc::log

extern "C" void xc_log_flush_line(const char* level, const char* msg) {
    xc::log::Level l = xc::log::Level::Info;
    if (level && level[0] == 'E') l = xc::log::Level::Error;
    else if (level && level[0] == 'W') l = xc::log::Level::Warn;
    xc::log::write(l, "%s", msg ? msg : "");
}

extern "C" void xc_log_raw(const char* msg) { xc::log::raw(msg); }

extern "C" int xc_log_is_ready(void) { return xc::log::fileReady(); }
