// SPDX-License-Identifier: GPL-3.0-only
// Display shim for PSBox UI on XCloud4: draw into a host-owned RGBA buffer.
#include "display/display.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace {
constexpr size_t kPixels = static_cast<size_t>(xc::display::kWidth) * xc::display::kHeight;
uint32_t *g_target = nullptr;
std::timed_mutex g_frame_mu;
}  // namespace

extern "C" void psbox_display_set_target(uint32_t *pixels) { g_target = pixels; }

namespace xc::display {

bool init(bool) { return true; }
void shutdown() { g_target = nullptr; }
void releaseForExit() {}
void holdFlips(bool) {}
void suspendForHome() {}
void resumeFromHome() {}
void resumeAfterRest() {}
std::timed_mutex &frameMutex() { return g_frame_mu; }
bool present() { return g_target != nullptr; }
bool streamOwnsDisplay() { return false; }

void drawRgba(const uint32_t *pixels) {
    if (!g_target || !pixels) return;
    std::memcpy(g_target, pixels, kPixels * sizeof(uint32_t));
}

bool drawYuv420(const uint8_t *, const uint8_t *, const uint8_t *, int, int, int, int, int, bool) {
    return false;
}
bool drawNv12(const uint8_t *, int, int, int, bool) { return false; }
bool fastScanout() { return false; }
void setOverlay(const uint32_t *, int, int, int, int, uint8_t) {}
void setSharpness(int) {}
void setDeband(int) {}
void setUpscaler(int) {}
void setRestore(bool) {}
uint64_t lastPresentId() { return 0; }
bool lastShown(uint64_t &, uint64_t &) { return false; }

bool readBackRgb(std::vector<uint8_t> &rgb, int &width, int &height) {
    if (!g_target) return false;
    width = static_cast<int>(kWidth);
    height = static_cast<int>(kHeight);
    rgb.resize(kPixels * 3);
    for (size_t i = 0; i < kPixels; ++i) {
        uint32_t p = g_target[i];
        rgb[i * 3 + 0] = static_cast<uint8_t>(p & 0xff);
        rgb[i * 3 + 1] = static_cast<uint8_t>((p >> 8) & 0xff);
        rgb[i * 3 + 2] = static_cast<uint8_t>((p >> 16) & 0xff);
    }
    return true;
}

}  // namespace xc::display
