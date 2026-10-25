// SPDX-License-Identifier: GPL-3.0-only
// PSBox UI host on XCloud4: AppUi + settings + stream menu + local update +
// crash guard + screenshot + report. Auth/catalog/session from X4 snapshots
// (no tokens). Cover art from X4 public product metadata.
#include "psbox_ui.h"

#include "app/crash_guard.h"
#include "app/local_update.h"
#include "app/report.h"
#include "app/screenshot.h"
#include "app/settings.h"
#include "app/update_check.h"
#include "display/display.h"
#include "net/http.h"
#include "platform/platform.h"
#include "ui/app_ui.h"
#include "ui/font.h"
#include "ui/image_cache.h"
#include "ui/stream_menu.h"
#include "ui/strings.h"
#include "util/log.h"

#include "../auth/xbox_catalog.h"
#include "../auth/xbox_session.h"
#include "../auth/device_auth.h"
#include "../ui/product_metadata.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::unique_ptr<xc::ui::Fonts> g_fonts;
std::unique_ptr<xc::ui::ImageCache> g_images;
std::unique_ptr<xc::ui::AppUi> g_ui;
std::unique_ptr<xc::ui::StreamMenu> g_stream_menu;
xc::app::Settings g_settings;
xc::ui::Canvas g_canvas(1920, 1080);
xc::ui::Canvas g_overlay(xc::ui::StreamMenu::kMenuW, xc::ui::StreamMenu::kMenuH);
std::mutex g_mu;
bool g_http_ok = false;
bool g_update_prompted = false;
bool g_first_stream_shot = false;
std::string g_last_code, g_last_uri;
enum X4AuthState g_last_auth = X4_AUTH_IDLE;
enum X4SessionState g_last_session = X4_SESSION_IDLE;
unsigned g_last_catalog_count = 0;
bool g_stream_menu_open = false;

X4CatalogSnapshot g_catalog{};
X4RecentSnapshot g_recent{};
bool g_have_catalog = false;
bool g_have_recent = false;

struct MetaEnrich {
    std::string name, publisher, description, cover, category;
};
std::map<std::string, MetaEnrich> g_meta;

std::string settingsPath() { return xc::platform::dataDir() + "/settings.json"; }

xc::ui::SettingsChoice choiceFromSettings(const xc::app::Settings &s) {
    xc::ui::SettingsChoice c;
    c.language = static_cast<int>(xc::ui::languageFromCode(s.language));
    c.resolution = s.resolution == "720p" ? 1 : (s.resolution == "1440p" || s.resolution == "best") ? 2 : 0;
    c.region = s.region;
    c.loginCountry = s.loginCountry;
    c.deadzone[0] = std::clamp(s.deadzoneLeft, 0, xc::ui::kMaxDeadzone);
    c.deadzone[1] = std::clamp(s.deadzoneRight, 0, xc::ui::kMaxDeadzone);
    c.triggerStrength = s.triggerStrength;
    c.triggerHz = s.triggerHz;
    c.triggerResistance = s.triggerResistance;
    c.triggerPulses = s.triggerPulses;
    c.circleConfirms = s.circleConfirms;
    c.lightBarMode = s.lightBarMode;
    c.lightBarColour = s.lightBarColour;
    c.sharpness = s.sharpness;
    c.deband = s.deband;
    c.upscaler = s.upscaler;
    c.highRefresh = s.highRefresh;
    c.saveScreenshots = s.saveScreenshots;
    c.maxBitrateMbps = s.maxBitrateMbps;
    c.gameVolume = s.gameVolume;
    return c;
}

void applyChoice(xc::app::Settings &s, const xc::ui::SettingsChoice &c) {
    s.language = xc::ui::languageCode(static_cast<xc::ui::Language>(c.language));
    s.resolution = c.resolution == 1 ? "720p" : c.resolution == 2 ? "best" : "1080p";
    s.region = c.region;
    s.loginCountry = c.loginCountry;
    s.deadzoneLeft = c.deadzone[0];
    s.deadzoneRight = c.deadzone[1];
    s.triggerStrength = c.triggerStrength;
    s.triggerHz = c.triggerHz;
    s.triggerResistance = c.triggerResistance;
    s.triggerPulses = c.triggerPulses;
    s.circleConfirms = c.circleConfirms;
    s.lightBarMode = c.lightBarMode;
    s.lightBarColour = c.lightBarColour;
    s.sharpness = c.sharpness;
    s.deband = c.deband;
    s.upscaler = c.upscaler;
    s.highRefresh = c.highRefresh;
    s.saveScreenshots = c.saveScreenshots;
    s.maxBitrateMbps = c.maxBitrateMbps;
    s.gameVolume = c.gameVolume;
}

xc::ui::GameTile tileFromTitle(const X4CatalogTitle &t) {
    xc::ui::GameTile g;
    g.productId = t.product_id;
    g.titleId = t.id;
    g.name = t.name[0] ? t.name : t.id;
    g.playable = t.entitled != 0;
    g.purchasable = t.entitled == 0;
    auto it = g_meta.find(t.product_id);
    if (it != g_meta.end()) {
        if (!it->second.name.empty()) g.name = it->second.name;
        g.publisher = it->second.publisher;
        g.description = it->second.description;
        g.tileUrl = it->second.cover;
        g.heroUrl = it->second.cover;
        if (!it->second.category.empty()) g.categories = {it->second.category};
    }
    return g;
}

void publishCatalog() {
    if (!g_ui || !g_have_catalog) return;
    const X4CatalogSnapshot *catalog = &g_catalog;
    const X4RecentSnapshot *recent = g_have_recent ? &g_recent : nullptr;
    std::vector<xc::ui::GameTile> all, owned, buy;
    all.reserve(catalog->count);
    for (unsigned i = 0; i < catalog->count; ++i) {
        auto tile = tileFromTitle(catalog->titles[i]);
        all.push_back(tile);
        if (tile.playable) owned.push_back(tile);
        else buy.push_back(tile);
    }
    std::vector<xc::ui::GameRow> rows;
    if (recent && recent->count > 0 && catalog->state == X4_CATALOG_READY) {
        xc::ui::GameRow rec;
        rec.title = "Recent";
        for (unsigned i = 0; i < recent->count; ++i) {
            for (unsigned j = 0; j < catalog->count; ++j) {
                if (std::strcmp(catalog->titles[j].id, recent->title_ids[i]) == 0) {
                    rec.tiles.push_back(tileFromTitle(catalog->titles[j]));
                    break;
                }
            }
        }
        if (!rec.tiles.empty()) rows.push_back(std::move(rec));
    }
    xc::ui::GameRow pass;
    pass.title = "Game Pass / cloud";
    pass.tiles = all;
    rows.push_back(std::move(pass));
    if (!owned.empty()) {
        xc::ui::GameRow yours;
        yours.title = "Your games";
        yours.tiles = owned;
        rows.push_back(std::move(yours));
    }
    g_ui->setRows(std::move(rows));
    g_ui->setOwned(owned, buy, catalog->state == X4_CATALOG_READY);
    g_ui->setSearchPools(all, owned);
    g_ui->setMembership(!owned.empty());
    g_ui->setLoading(catalog->state == X4_CATALOG_LOADING, catalog->count, catalog->total);
}

#ifdef X4_OPENORBIS
#include <orbis/Pad.h>
#endif

xc::ui::NavInput navFromPad(uint32_t pressed, uint32_t held, float lx, float ly, float rx, float ry, float l2,
                            float r2, uint64_t now_ms, bool circleConfirms) {
    xc::ui::NavInput n{};
    n.nowMs = now_ms;
    n.up = (pressed & ORBIS_PAD_BUTTON_UP) != 0;
    n.down = (pressed & ORBIS_PAD_BUTTON_DOWN) != 0;
    n.left = (pressed & ORBIS_PAD_BUTTON_LEFT) != 0;
    n.right = (pressed & ORBIS_PAD_BUTTON_RIGHT) != 0;
    const bool cross = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
    const bool circle = (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0;
    n.accept = circleConfirms ? circle : cross;
    n.back = circleConfirms ? cross : circle;
    n.options = (pressed & ORBIS_PAD_BUTTON_OPTIONS) != 0;
    n.square = (pressed & ORBIS_PAD_BUTTON_SQUARE) != 0;
    n.triangle = (pressed & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
    n.l1 = (pressed & ORBIS_PAD_BUTTON_L1) != 0;
    n.r1 = (pressed & ORBIS_PAD_BUTTON_R1) != 0;
    n.l2 = l2 > 0.5f;
    n.r2 = r2 > 0.5f;
    n.r3 = (pressed & ORBIS_PAD_BUTTON_R3) != 0;
    n.touchpad = (held & ORBIS_PAD_BUTTON_TOUCH_PAD) != 0;
    n.stickX = lx;
    n.stickY = ly;
    n.rawLX = lx;
    n.rawLY = ly;
    n.rawRX = rx;
    n.rawRY = ry;
    n.l2Analog = l2;
    n.r2Analog = r2;
    n.dpadLeft = n.left;
    n.dpadRight = n.right;
    return n;
}

void blitOverlay(uint32_t *dst, const xc::ui::Canvas &src, int x, int y) {
    if (!dst) return;
    for (int row = 0; row < src.height(); ++row) {
        int dy = y + row;
        if (dy < 0 || dy >= 1080) continue;
        for (int col = 0; col < src.width(); ++col) {
            int dx = x + col;
            if (dx < 0 || dx >= 1920) continue;
            uint32_t p = src.data()[row * src.width() + col];
            if ((p >> 24) == 0) continue;
            dst[dy * 1920 + dx] = p;
        }
    }
}

void persistStreamMenu() {
    if (!g_stream_menu) return;
    bool dirty = false;
    int res = g_stream_menu->resolution();
    std::string want = res == 1 ? "720p" : res == 2 ? "best" : "1080p";
    if (g_settings.resolution != want) {
        g_settings.resolution = want;
        dirty = true;
    }
    if (g_settings.streamStats != g_stream_menu->statsOn()) {
        g_settings.streamStats = g_stream_menu->statsOn();
        dirty = true;
    }
    if (g_settings.sharpness != g_stream_menu->sharpness()) {
        g_settings.sharpness = g_stream_menu->sharpness();
        dirty = true;
    }
    if (g_settings.deband != g_stream_menu->deband()) {
        g_settings.deband = g_stream_menu->deband();
        dirty = true;
    }
    if (g_settings.upscaler != g_stream_menu->upscaler()) {
        g_settings.upscaler = g_stream_menu->upscaler();
        dirty = true;
    }
    if (g_settings.triggerStrength != g_stream_menu->triggerStrength()) {
        g_settings.triggerStrength = g_stream_menu->triggerStrength();
        dirty = true;
    }
    if (g_settings.deadzoneLeft != g_stream_menu->deadzone()) {
        g_settings.deadzoneLeft = g_settings.deadzoneRight = g_stream_menu->deadzone();
        dirty = true;
    }
    if (g_settings.circleConfirms != g_stream_menu->circleConfirms()) {
        g_settings.circleConfirms = g_stream_menu->circleConfirms();
        dirty = true;
        if (g_ui) g_ui->setCircleConfirms(g_settings.circleConfirms);
    }
    if (dirty) g_settings.save(settingsPath());
}

std::string readLogFileTail() {
    std::string text;
    xc::platform::readFile(xc::platform::dataDir() + "/xcloud.log", text);
    if (text.size() > 64u * 1024u) text.erase(0, text.size() - 64u * 1024u);
    return text;
}

void handleReport(const std::string &what) {
    if (!g_ui) return;
    xc::app::report::Info info;
    info.title = "PSBox 01.57 report";
    info.what = what.empty() ? "User report from Settings" : what;
    info.release = "01.57";
    info.firmware = xc::platform::firmwareVersion();
    info.log = xc::app::report::logExcerpt(readLogFileTail(), false);
    std::string url = xc::app::report::issueUrl(info);
    g_ui->showReport(url, "Report a problem");
}

}  // namespace

extern "C" int psbox_ui_init(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_ui) return 0;
    if (!xc::platform::init()) {
        XC_LOGE("psbox: platform init failed");
        return -1;
    }
    g_http_ok = xc::net::initTls(xc::platform::caBundlePath());
    if (!g_http_ok) XC_LOGW("psbox: TLS init failed; box art may be empty (%s)", xc::net::tlsInitError().c_str());

    g_settings.load(settingsPath());
    xc::log::setFile((xc::platform::dataDir() + "/xcloud.log").c_str());
    xc::ui::setLanguage(xc::ui::languageFromCode(g_settings.language));
    xc::app::shot::setEnabled(g_settings.saveScreenshots);

    auto prev = xc::app::crash::takePrevious(xc::platform::dataDir() + "/crash.marker");
    g_fonts = std::make_unique<xc::ui::Fonts>();
    if (!g_fonts->load(xc::platform::assetDir() + "/fonts")) {
        XC_LOGE("psbox: Inter fonts missing under %s/fonts", xc::platform::assetDir().c_str());
        return -2;
    }
    g_images = std::make_unique<xc::ui::ImageCache>(
        [] {
            if (g_ui) g_ui->invalidate();
        },
        160u << 20, xc::platform::dataDir() + "/imgcache");
    g_ui = std::make_unique<xc::ui::AppUi>(*g_fonts, *g_images);
    g_stream_menu = std::make_unique<xc::ui::StreamMenu>(*g_fonts);
    auto choice = choiceFromSettings(g_settings);
    g_ui->setSettings(choice);
    g_ui->setCircleConfirms(choice.circleConfirms);
    g_ui->setPrefs(g_settings.hidden,
                   g_settings.librarySort == "az"         ? xc::ui::LibrarySort::AZ
                   : g_settings.librarySort == "console" ? xc::ui::LibrarySort::Console
                                                         : xc::ui::LibrarySort::Recent,
                   g_settings.favorites);
    g_ui->setPerGameSettings(g_settings.perGame);
    g_ui->setUpdateState("01.57", {}, false);
    g_ui->showSplash("PSBox");
    if (prev.found) {
        g_ui->showToast(std::string("Previous session: ") + prev.kind);
        XC_LOGW("psbox: previous crash marker kind=%s stage=%s", prev.kind.c_str(), prev.stage.c_str());
    }
    xc::app::crash::install(xc::platform::dataDir() + "/crash.marker");
    xc::app::crash::startWatchdog(15000);
    XC_LOGI("psbox: UI init ok (XCloud4 transport)");
    return 0;
}

extern "C" void psbox_ui_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_images) g_images->shutdownThreads();
    g_stream_menu.reset();
    g_ui.reset();
    g_images.reset();
    g_fonts.reset();
    if (g_http_ok) xc::net::shutdownTls();
    g_http_ok = false;
    xc::platform::shutdown();
}

extern "C" void psbox_ui_set_auth(const X4AuthSnapshot *auth) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_ui || !auth) return;
    if (auth->state == X4_AUTH_WAITING && auth->user_code[0]) {
        if (g_last_code != auth->user_code || g_last_uri != auth->verification_uri) {
            g_last_code = auth->user_code;
            g_last_uri = auth->verification_uri;
            g_ui->showSignIn(g_last_code, g_last_uri);
        }
    } else if (auth->state == X4_AUTH_AUTHORIZED && g_last_auth != X4_AUTH_AUTHORIZED) {
        g_ui->showHome();
    } else if (auth->state == X4_AUTH_ERROR || auth->state == X4_AUTH_DENIED) {
        g_ui->showError(auth->stage[0] ? auth->stage : "Sign-in error");
    } else if (auth->state == X4_AUTH_CONNECTING || auth->state == X4_AUTH_CONNECTED) {
        g_ui->showSplash(auth->stage[0] ? auth->stage : "Connecting…");
    }
    g_last_auth = auth->state;
    xc::app::crash::setStage(auth->stage[0] ? auth->stage : "auth");
}

extern "C" void psbox_ui_set_catalog(const X4CatalogSnapshot *catalog) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!catalog) return;
    g_catalog = *catalog;
    g_have_catalog = true;
    if (catalog->count == g_last_catalog_count && catalog->state == X4_CATALOG_READY && g_meta.empty()) return;
    g_last_catalog_count = catalog->count;
    publishCatalog();
    if (catalog->state == X4_CATALOG_READY && g_ui && g_ui->screen() != xc::ui::Screen::Streaming &&
        g_ui->screen() != xc::ui::Screen::Launching)
        g_ui->showHome();
}

extern "C" void psbox_ui_set_recent(const X4RecentSnapshot *recent) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!recent) return;
    g_recent = *recent;
    g_have_recent = true;
    publishCatalog();
}

extern "C" void psbox_ui_ingest_metadata(const X4ProductMetadata *items, size_t count) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!items || !count) return;
    bool changed = false;
    for (size_t i = 0; i < count; ++i) {
        if (!items[i].product_id[0]) continue;
        MetaEnrich &m = g_meta[items[i].product_id];
        if (items[i].name[0] && m.name != items[i].name) {
            m.name = items[i].name;
            changed = true;
        }
        if (items[i].publisher[0]) m.publisher = items[i].publisher;
        if (items[i].description[0]) m.description = items[i].description;
        if (items[i].category[0]) m.category = items[i].category;
        if (items[i].cover_url[0] && m.cover != items[i].cover_url) {
            m.cover = items[i].cover_url;
            changed = true;
        }
    }
    if (changed) publishCatalog();
}

extern "C" void psbox_ui_set_session(const X4SessionSnapshot *session) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_ui || !session) return;
    if (session->state == X4_SESSION_STREAMING && g_last_session != X4_SESSION_STREAMING) {
        g_ui->showStreaming();
        g_stream_menu_open = false;
        g_first_stream_shot = false;
        if (g_stream_menu) g_stream_menu->close();
        xc::app::crash::setStreamingWatch(true);
    } else if (session->state == X4_SESSION_STARTING || session->state == X4_SESSION_WAITING ||
               session->state == X4_SESSION_READY || session->state == X4_SESSION_AUTHORIZING ||
               session->state == X4_SESSION_AUTHORIZED || session->state == X4_SESSION_NEGOTIATING ||
               session->state == X4_SESSION_CONNECTING) {
        xc::ui::GameTile t;
        t.name = session->title_name[0] ? session->title_name : "Game";
        g_ui->showLaunching(t, session->stage[0] ? session->stage : "Starting…");
        g_ui->setLaunchStatus(session->stage);
    } else if (session->state == X4_SESSION_ERROR) {
        g_ui->showError(session->stage[0] ? session->stage : "Session failed");
        xc::app::crash::setStreamingWatch(false);
    } else if ((session->state == X4_SESSION_IDLE || session->state == X4_SESSION_CLOSED ||
                session->state == X4_SESSION_CANCELLED) &&
               g_last_session == X4_SESSION_STREAMING) {
        g_ui->showHome();
        xc::app::crash::setStreamingWatch(false);
    }
    g_last_session = session->state;
    if (session->stage[0]) xc::app::crash::setStage(session->stage);
}

extern "C" int psbox_ui_handle(uint32_t pressed, uint32_t held, float lx, float ly, float rx, float ry, float l2,
                              float r2, uint64_t now_ms, char *title_id_out, size_t title_cap) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (title_id_out && title_cap) title_id_out[0] = 0;
    if (!g_ui) return 0;
    xc::app::crash::heartbeat();
    auto nav = navFromPad(pressed, held, lx, ly, rx, ry, l2, r2, now_ms, g_settings.circleConfirms);
    xc::app::shot::pollMenu(nav.l1, nav.r1, nav.square, true, false, "menu");
    xc::ui::UiEvent ev = g_ui->handle(nav);
    if (ev.action == xc::ui::Action::SettingsChanged) {
        applyChoice(g_settings, ev.settings);
        g_settings.save(settingsPath());
        g_ui->setCircleConfirms(ev.settings.circleConfirms);
        xc::app::shot::setEnabled(g_settings.saveScreenshots);
        xc::ui::setLanguage(xc::ui::languageFromCode(g_settings.language));
    }
    if (ev.action == xc::ui::Action::PrefsChanged) {
        g_settings.hidden = ev.hidden;
        g_settings.favorites = ev.favorites;
        g_settings.librarySort = ev.librarySort == xc::ui::LibrarySort::AZ         ? "az"
                                 : ev.librarySort == xc::ui::LibrarySort::Console ? "console"
                                                                                  : "recent";
        g_settings.save(settingsPath());
    }
    if (ev.action == xc::ui::Action::GameSettingsChanged && ev.hasGameProfile) {
        const std::string key = !ev.game.productId.empty() ? ev.game.productId : ev.game.titleId;
        if (!key.empty()) {
            g_settings.perGame[key] = ev.gameProfile;
            g_settings.save(settingsPath());
            g_ui->setPerGameSettings(g_settings.perGame);
        }
    }
    if (ev.action == xc::ui::Action::UpdateNow) {
        auto r = xc::app::installLocalUpdate("01.57");
        g_ui->showToast(r.detail.empty() ? "Update" : r.detail);
    }
    if (ev.action == xc::ui::Action::UpdateLater) {
        g_settings.skippedUpdate = g_ui ? "01.57" : "";
        g_settings.save(settingsPath());
    }
    if (ev.action == xc::ui::Action::CheckUpdate) {
        auto found = xc::app::findLocalUpdate("01.57");
        if (found.status == xc::app::LocalUpdateStatus::Available) {
            g_ui->setUpdateState("01.57", found.version, false);
            g_ui->offerUpdate();
        } else {
            xc::app::Release rel;
            std::string err;
            if (xc::app::findLatestRelease(rel, err) && xc::app::isNewerVersion(rel.tag, "01.57")) {
                g_ui->showToast(std::string("GitHub: ") + rel.tag + " — copy pkg to /data/psbox/update/");
            } else {
                g_ui->showToast(err.empty() ? "No update found" : err);
            }
        }
    }
    if (ev.action == xc::ui::Action::Report) {
        handleReport(ev.reportWhat);
    }
    if (ev.action == xc::ui::Action::SendLog) {
        handleReport("Send previous session log");
    }
    if (ev.action == xc::ui::Action::CancelLaunch) return -1;
    if (ev.action == xc::ui::Action::Play && title_id_out && title_cap > 1 && !ev.game.titleId.empty()) {
        std::snprintf(title_id_out, title_cap, "%s", ev.game.titleId.c_str());
        return 1;
    }
    return 0;
}

extern "C" void psbox_ui_draw(uint32_t *pixels, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_ui || !pixels) return;
    psbox_display_set_target(pixels);
    g_ui->render(g_canvas, now_ms);
    xc::display::drawRgba(g_canvas.data());
    xc::app::shot::afterPresent();
}

extern "C" int psbox_ui_stream_menu(uint32_t *pixels, uint32_t pressed, uint32_t held, uint64_t now_ms, int video_w,
                                   int video_h, int fps, int bitrate_kbps) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_stream_menu || !pixels) return 0;
    xc::app::crash::heartbeat();
    xc::app::crash::videoHeartbeat();
    psbox_display_set_target(pixels);
    if (!g_first_stream_shot && g_settings.saveScreenshots) {
        xc::app::shot::request(xc::app::shot::Kind::FirstStream, true);
        g_first_stream_shot = true;
    }
    xc::ui::NavInput nav = navFromPad(pressed, held, 0, 0, 0, 0, 0, 0, now_ms, g_settings.circleConfirms);
    if (g_stream_menu_open) {
        xc::app::shot::pollMenu(nav.l1, nav.r1, nav.square, true, true, "stream-menu");
    }
    if (!g_stream_menu_open && (nav.options && nav.touchpad)) {
        g_stream_menu->open(choiceFromSettings(g_settings).resolution, g_settings.streamStats, g_settings.sharpness,
                            g_settings.deband, g_settings.upscaler, false, true, false, g_settings.triggerStrength,
                            g_settings.deadzoneLeft, g_settings.circleConfirms);
        g_stream_menu_open = true;
    }
    if (!g_stream_menu_open) {
        xc::app::shot::afterPresent();
        return 0;
    }
    auto act = g_stream_menu->handle(nav);
    if (act == xc::ui::MenuAction::SendLog) handleReport("Send log from stream menu");
    xc::ui::StreamInfo info;
    info.width = video_w;
    info.height = video_h;
    info.fps = fps;
    info.mbps = bitrate_kbps / 1000.0;
    auto menu = g_stream_menu->renderMenu(info);
    blitOverlay(pixels, menu, xc::ui::StreamMenu::kMenuX, xc::ui::StreamMenu::kMenuY);
    if (g_stream_menu->statsOn()) {
        auto stats = g_stream_menu->renderStats(info);
        blitOverlay(pixels, stats, xc::ui::StreamMenu::kStatsX, xc::ui::StreamMenu::kStatsY);
    }
    persistStreamMenu();
    xc::app::shot::afterPresent();
    if (act == xc::ui::MenuAction::Leave) {
        g_stream_menu->close();
        g_stream_menu_open = false;
        return 2;
    }
    if (act == xc::ui::MenuAction::Close) {
        g_stream_menu->close();
        g_stream_menu_open = false;
    }
    return 1;
}

extern "C" void psbox_ui_poll_local_update(const char *current_app_ver) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_ui || g_update_prompted) return;
    const char *ver = current_app_ver ? current_app_ver : "01.57";
    auto found = xc::app::findLocalUpdate(ver);
    if (found.status == xc::app::LocalUpdateStatus::Available) {
        if (!g_settings.skippedUpdate.empty() && g_settings.skippedUpdate == found.version) return;
        g_ui->setUpdateState(ver, found.version, false);
        g_ui->offerUpdate();
        g_update_prompted = true;
    }
}

extern "C" int psbox_ui_install_crash_guard(void) {
    xc::app::crash::install(xc::platform::dataDir() + "/crash.marker");
    return 0;
}
