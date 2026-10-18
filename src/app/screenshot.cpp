// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/screenshot.h"

#include "display/display.h"
#include "platform/platform.h"
#include "util/log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace xc::app::shot {
namespace {

constexpr const char* kDir = "/data/psbox/screenshots";
constexpr int kDeferPresents = 2;  // aspetta frame pieni prima dell'auto-shot
std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_autoMask{0};  // bit per Kind auto già fatti in sessione
std::mutex g_mu;
bool g_busy = false;
uint64_t g_comboSince = 0;
std::string g_lastScreen;

// Prenotazione: kind da catturare dopo N present (0 = pronto al prossimo afterPresent).
Kind g_pendingKind = Kind::Manual;
int g_pendingCountdown = -1;  // <0 = niente in coda
bool g_pendingAllowStream = false;

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Manual: return "menu";
        case Kind::SignIn: return "signin";
        case Kind::Loading: return "loading";
        case Kind::Library: return "library";
        case Kind::GameDetails: return "details";
        case Kind::Connecting: return "connecting";
        case Kind::Settings: return "settings";
        case Kind::FirstStream: return "stream";
        case Kind::UpdatePrompt: return "update";
    }
    return "shot";
}

int kindBit(Kind k) { return 1 << static_cast<int>(k); }

// Blur box 9×9 sull'angolo profilo (gamertag): privacy senza rettangolo nero.
// Opzionale fascia centrale solo su SignIn (codice login), sempre blur.
void redactPrivacy(std::vector<uint8_t>& rgb, int w, int h, Kind kind) {
    auto blurRect = [&](int x0, int y0, int x1, int y1) {
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        if (x1 <= x0 || y1 <= y0) return;
        const int rw = x1 - x0, rh = y1 - y0;
        std::vector<uint8_t> tmp(static_cast<size_t>(rw) * rh * 3);
        constexpr int kR = 4;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                int rs = 0, gs = 0, bs = 0, n = 0;
                for (int dy = -kR; dy <= kR; ++dy) {
                    int yy = y + dy;
                    if (yy < y0 || yy >= y1) continue;
                    const uint8_t* row = rgb.data() + static_cast<size_t>(yy) * w * 3;
                    for (int dx = -kR; dx <= kR; ++dx) {
                        int xx = x + dx;
                        if (xx < x0 || xx >= x1) continue;
                        const uint8_t* p = row + xx * 3;
                        rs += p[0];
                        gs += p[1];
                        bs += p[2];
                        ++n;
                    }
                }
                uint8_t* o = tmp.data() + (static_cast<size_t>(y - y0) * rw + (x - x0)) * 3;
                if (n > 0) {
                    o[0] = static_cast<uint8_t>(rs / n);
                    o[1] = static_cast<uint8_t>(gs / n);
                    o[2] = static_cast<uint8_t>(bs / n);
                }
            }
        }
        for (int y = 0; y < rh; ++y) {
            std::memcpy(rgb.data() + (static_cast<size_t>(y0 + y) * w + x0) * 3,
                        tmp.data() + static_cast<size_t>(y) * rw * 3, static_cast<size_t>(rw) * 3);
        }
    };
    // Chip profilo in alto a destra (sempre, tranne stream).
    blurRect(w - 420, 20, w - 40, 110);
    // Codice login al centro: solo schermata SignIn (non library/update).
    if (kind == Kind::SignIn) blurRect(w / 2 - 280, h / 2 - 40, w / 2 + 280, h / 2 + 120);
}

struct PngBuf {
    std::vector<uint8_t> data;
};

void pngWrite(void* ctx, void* data, int size) {
    auto* b = static_cast<PngBuf*>(ctx);
    auto* p = static_cast<const uint8_t*>(data);
    b->data.insert(b->data.end(), p, p + size);
}

bool writePng(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    PngBuf buf;
    buf.data.reserve(static_cast<size_t>(w) * h / 4);
    if (!stbi_write_png_to_func(pngWrite, &buf, w, h, 3, rgb.data(), w * 3)) return false;
    return platform::writeFileAtomic(path, std::string(reinterpret_cast<const char*>(buf.data.data()), buf.data.size()));
}

void encodeAndSave(Kind kind, std::vector<uint8_t> rgb, int w, int h) {
    if (kind != Kind::FirstStream) redactPrivacy(rgb, w, h, kind);
    if (mkdir(kDir, 0755) != 0 && errno != EEXIST) {
        XC_LOGW("[SHOT] mkdir %s errno %d", kDir, errno);
    }
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char name[96];
    std::snprintf(name, sizeof name, "%s_%04d%02d%02d_%02d%02d%02d.png", kindName(kind), tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string path = std::string(kDir) + "/" + name;
    bool ok = writePng(path, rgb, w, h);
    XC_LOGI("[SHOT] %s %dx%d → %s (%s)", kindName(kind), w, h, path.c_str(), ok ? "ok" : "fail");
}

void saveFromStaging(Kind kind, std::vector<uint8_t> rgb, int w, int h) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_busy) {
            XC_LOGI("[SHOT] skip busy kind %s", kindName(kind));
            return;
        }
        g_busy = true;
    }
    platform::Thread th;
    if (!platform::startThread(
            th,
            [kind, rgb = std::move(rgb), w, h]() mutable {
                encodeAndSave(kind, std::move(rgb), w, h);
                std::lock_guard<std::mutex> lock(g_mu);
                g_busy = false;
            },
            2u << 20)) {
        encodeAndSave(kind, std::move(rgb), w, h);
        std::lock_guard<std::mutex> lock(g_mu);
        g_busy = false;
    } else {
        th.detach();
    }
}

bool claimAuto(Kind kind) {
    if (kind == Kind::Manual) return true;
    uint32_t bit = static_cast<uint32_t>(kindBit(kind));
    uint32_t prev = g_autoMask.load();
    if (prev & bit) return false;
    g_autoMask.fetch_or(bit);
    return true;
}

void enqueue(Kind kind, bool allowStream, int deferPresents) {
    if (!claimAuto(kind)) return;
    g_pendingKind = kind;
    g_pendingAllowStream = allowStream;
    g_pendingCountdown = deferPresents;
    XC_LOGI("[SHOT] in coda kind %s (dopo %d present)", kindName(kind), deferPresents);
}

Kind tagToKind(const char* tag) {
    if (!tag) return Kind::Manual;
    if (std::strcmp(tag, "signin") == 0 || std::strcmp(tag, "login") == 0) return Kind::SignIn;
    if (std::strcmp(tag, "loading") == 0 || std::strcmp(tag, "startup") == 0) return Kind::Loading;
    if (std::strcmp(tag, "library") == 0 || std::strcmp(tag, "home") == 0) return Kind::Library;
    if (std::strcmp(tag, "details") == 0 || std::strcmp(tag, "game") == 0) return Kind::GameDetails;
    if (std::strcmp(tag, "connecting") == 0 || std::strcmp(tag, "queue") == 0) return Kind::Connecting;
    if (std::strcmp(tag, "settings") == 0) return Kind::Settings;
    if (std::strcmp(tag, "stream") == 0) return Kind::FirstStream;
    if (std::strcmp(tag, "update") == 0) return Kind::UpdatePrompt;
    return Kind::Manual;
}

}  // namespace

void setEnabled(bool on) {
    g_on.store(on);
    XC_LOGI("[SHOT] opzione %s", on ? "on" : "off");
}

bool enabled() { return g_on.load(); }

void request(Kind kind, bool allowStream) {
    // FirstStream / request esterni: cattura al prossimo afterPresent (o subito se già in stream path).
    enqueue(kind, allowStream, kind == Kind::FirstStream ? 0 : kDeferPresents);
}

void pollMenu(bool l1, bool r1, bool square, bool inMenus, bool streaming, const char* screenTag) {
    if (!g_on.load()) {
        g_comboSince = 0;
        return;
    }
    // Auto una volta per schermo (sessione): prenota, non leggere il FB qui.
    if (screenTag && *screenTag) {
        if (g_lastScreen != screenTag) {
            g_lastScreen = screenTag;
            Kind k = tagToKind(screenTag);
            if (k == Kind::FirstStream) {
                if (streaming) enqueue(Kind::FirstStream, true, 0);
            } else if (!streaming && k != Kind::Manual) {
                // Loading: più defer per evitare metà-frame dello splash.
                int defer = (k == Kind::Loading) ? 4 : kDeferPresents;
                enqueue(k, false, defer);
            }
        }
    }
    if (!inMenus || streaming) {
        g_comboSince = 0;
        return;
    }
    // Combo libera: L1+R1+□ tenuti 1 s (L3/R3 restano preferiti).
    if (l1 && r1 && square) {
        uint64_t now = platform::nowMs();
        if (!g_comboSince) g_comboSince = now;
        if (now - g_comboSince >= 1000) {
            g_comboSince = now + 100000;  // anti-ripetizione
            XC_LOGI("[SHOT] combo L1+R1+□");
            enqueue(Kind::Manual, false, 0);
        }
    } else {
        g_comboSince = 0;
    }
}

void afterPresent() {
    if (!g_on.load() || g_pendingCountdown < 0) return;
    if (g_pendingCountdown > 0) {
        --g_pendingCountdown;
        return;
    }
    Kind kind = g_pendingKind;
    bool allowStream = g_pendingAllowStream;
    g_pendingCountdown = -1;
    if (kind == Kind::FirstStream && !allowStream) return;

    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    // Copia sync dal buffer appena presentato (stesso percorso VideoOut).
    if (!display::readBackRgb(rgb, w, h) || w <= 0 || h <= 0) {
        XC_LOGW("[SHOT] readBack fallito kind %s", kindName(kind));
        return;
    }
    XC_LOGI("[SHOT] cattura compositata kind %s %dx%d", kindName(kind), w, h);
    saveFromStaging(kind, std::move(rgb), w, h);
}

}  // namespace xc::app::shot
