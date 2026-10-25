// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/crash_log.h"

#include "net/http.h"
#include "platform/platform.h"
#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__has_include)
#if __has_include("../platform/ps4/secrets.h")
#include "../platform/ps4/secrets.h"
#endif
#endif
#ifndef PSBOX_WEBHOOK_KEY
#define PSBOX_WEBHOOK_KEY ""
#endif
#ifndef PSBOX_WEBHOOK_URL
#define PSBOX_WEBHOOK_URL ""
#endif

namespace xc::app {
namespace {

// URL e chiave solo in secrets.h (gitignored). Senza URL, Invia log è no-op.
constexpr const char* kUrl = PSBOX_WEBHOOK_URL;
constexpr size_t kMaxBytes = 200 * 1024;
/* Il webhook taglia il corpo intorno a 3,9 KB. Ogni POST resta sotto i 3 KB. */
constexpr size_t kChunk = 2976;

std::string g_prev;
std::atomic<bool> g_busy{false};

bool readTail(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long sz = std::ftell(f);
    if (sz <= 0) {
        std::fclose(f);
        return false;
    }
    long start = sz > static_cast<long>(kMaxBytes) ? sz - static_cast<long>(kMaxBytes) : 0;
    if (std::fseek(f, start, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(sz - start));
    size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(n);
    if (start > 0) {
        auto nl = out.find('\n');
        if (nl != std::string::npos) out.erase(0, nl + 1);
    }
    return !out.empty();
}

}  // namespace

bool preparePreviousLog() {
    std::string dir = platform::dataDir();
    std::string cur = dir + "/xcloud.log";
    g_prev = dir + "/xcloud.prev.log";
    struct stat st{};
    /* rename prima di setFile: il FILE* nuovo non è l'inode rinominato,
     * quindi il flush della sessione corrente non resta sul file precedente. */
    if (::stat(cur.c_str(), &st) == 0 && st.st_size > 0) std::rename(cur.c_str(), g_prev.c_str());
    return ::stat(g_prev.c_str(), &st) == 0 && st.st_size > 0;
}

std::vector<std::pair<size_t, size_t>> splitChunks(const std::string& text) {
    std::vector<std::pair<size_t, size_t>> parts;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t n = std::min(kChunk, text.size() - pos);
        if (pos + n < text.size()) {
            size_t nl = text.rfind('\n', pos + n - 1);
            if (nl != std::string::npos && nl >= pos && nl + 1 > pos) n = nl - pos + 1;
        }
        if (n == 0) n = 1;
        parts.emplace_back(pos, n);
        pos += n;
    }
    return parts;
}

void sendPreviousLog() {
    if (g_busy.exchange(true)) return;
    std::string path = g_prev.empty() ? platform::dataDir() + "/xcloud.prev.log" : g_prev;
    std::thread([path] {
        bool ok = false;
        std::string body;
        if (kUrl[0] != '\0' && PSBOX_WEBHOOK_KEY[0] != '\0' && readTail(path, body)) {
            auto parts = splitChunks(body);
            const int n = static_cast<int>(parts.size());
            /* Prima la fine del file: se un POST successivo non parte, il crash c'è già. */
            for (int i = n - 1; i >= 0; --i) {
                auto [off, len] = parts[static_cast<size_t>(i)];
                net::Request req;
                req.method = "POST";
                req.url = kUrl;
                req.headers.push_back({"Content-Type", "text/plain"});
                req.headers.push_back({"User-Agent", "xCloud-PS4/" XC_APP_VERSION});
                req.headers.push_back({"Authorization", std::string("Bearer ") + PSBOX_WEBHOOK_KEY});
                req.body = "parte " + std::to_string(i + 1) + " di " + std::to_string(n) + "\n";
                req.body.append(body, off, len);
                req.timeoutMs = 20000;
                net::Response resp = net::perform(req);
                if (i == n - 1) ok = resp.ok();
                XC_LOGI("invio log: parte %d di %d HTTP %d, %zu byte%s%s", i + 1, n, resp.status, req.body.size(),
                        resp.error.empty() ? "" : " ", resp.error.c_str());
            }
        } else {
            XC_LOGI("invio log: disattivato o file assente");
        }
        platform::notify(ok ? "Log inviato" : "Invio fallito", ok ? "log inviato" : "invio log fallito");
        g_busy = false;
    }).detach();
}

}  // namespace xc::app
