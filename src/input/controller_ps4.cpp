// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// DualShock 4 via scePad (OpenOrbis). Stessa mappatura Xbox di controller.cpp
// sulla PS5. Niente grilletti adattivi DualSense: setTriggerRumble è un no-op
// (il DS4 non li ha). Lightbar e vibrazione restano.
#include "input/controller.h"
#include "input/tuning.h"
#include "util/log.h"

#include "platform/platform.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

namespace xc::input {

namespace {
struct Pad {
    int32_t userId = ORBIS_USER_SERVICE_USER_ID_INVALID;
    int32_t handle = -1;
    std::atomic<bool> connected{false};
    char name[32] = {};
    uint16_t touchW = 1920;
    uint16_t touchH = 942;
    std::atomic<uint32_t> rumble{0};
    std::atomic<uint64_t> rumbleUntil{0};
    uint32_t rumbleApplied = 0;
    std::atomic<uint32_t> light{0};
    uint32_t lightApplied = 0;
};
Pad g_pads[kMaxPads];
std::atomic<float> g_deadzone[2] = {0.15f, 0.15f};
std::atomic<bool> g_circle{false};

float normStick(uint8_t v) { return (static_cast<int>(v) - 128) / 128.0f; }
float normTrigger(uint8_t v) { return v / 255.0f; }

void closePad(Pad& pad) {
    if (pad.handle >= 0) {
        OrbisPadVibeParam off{};
        scePadSetVibration(pad.handle, &off);
        scePadResetLightBar(pad.handle);
        scePadClose(pad.handle);
    }
    pad.handle = -1;
    pad.userId = ORBIS_USER_SERVICE_USER_ID_INVALID;
    pad.connected = false;
    pad.name[0] = 0;
}

bool openPad(Pad& pad, int32_t userId) {
    int32_t h = scePadOpen(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (h < 0) {
        XC_LOGW("scePadOpen(utente %d): 0x%08x", userId, static_cast<unsigned>(h));
        return false;
    }
    pad.userId = userId;
    pad.handle = h;
    if (sceUserServiceGetUserName(userId, pad.name, sizeof pad.name) != 0) pad.name[0] = 0;
    OrbisPadInformation info{};
    if (scePadGetControllerInformation(h, &info) == 0 && info.touchResolutionX && info.touchResolutionY) {
        pad.touchW = info.touchResolutionX;
        pad.touchH = info.touchResolutionY;
    }
    XC_LOGI("pad aperto utente %d (%s), touch %ux%u", userId, pad.name[0] ? pad.name : "?", pad.touchW, pad.touchH);
    return true;
}

void applyRumble(Pad& pad) {
    uint32_t want = pad.rumble.load();
    uint64_t until = pad.rumbleUntil.load();
    if (want && until && platform::nowMs() >= until) {
        pad.rumble.compare_exchange_strong(want, 0);
        want = 0;
    }
    if (want == pad.rumbleApplied) return;
    pad.rumbleApplied = want;
    OrbisPadVibeParam p{static_cast<uint8_t>(want >> 8), static_cast<uint8_t>(want)};
    scePadSetVibration(pad.handle, &p);
}

void applyLight(Pad& pad) {
    uint32_t want = pad.light.load();
    if (want == pad.lightApplied) return;
    pad.lightApplied = want;
    if ((want & 0x01000000) == 0) {
        scePadResetLightBar(pad.handle);
        return;
    }
    OrbisPadColor c{static_cast<uint8_t>(want >> 16), static_cast<uint8_t>(want >> 8), static_cast<uint8_t>(want), 0};
    scePadSetLightBar(pad.handle, &c);
}
}  // namespace

bool init() {
    OrbisUserServiceInitializeParams param{700};
    sceUserServiceInitialize(&param);
    int rc = scePadInit();
    if (rc < 0) {
        XC_LOGE("scePadInit: 0x%08x", static_cast<unsigned>(rc));
        return false;
    }
    int32_t uid = ORBIS_USER_SERVICE_USER_ID_INVALID;
    rc = sceUserServiceGetInitialUser(&uid);
    if (rc != 0 || uid < 0) {
        XC_LOGE("sceUserServiceGetInitialUser: 0x%08x", static_cast<unsigned>(rc));
        return false;
    }
    return openPad(g_pads[0], uid);
}

void shutdown() {
    for (auto& p : g_pads) closePad(p);
}

void refreshPads() {
    OrbisUserServiceLoginUserIdList list{};
    for (auto& u : list.userId) u = ORBIS_USER_SERVICE_USER_ID_INVALID;
    if (sceUserServiceGetLoginUserIdList(&list) != 0) return;
    for (int i = 1; i < kMaxPads; ++i) {
        if (g_pads[i].handle < 0) continue;
        bool still = false;
        for (int32_t u : list.userId)
            if (u == g_pads[i].userId) still = true;
        if (!still) closePad(g_pads[i]);
    }
    for (int32_t user : list.userId) {
        if (user < 0) continue;
        bool known = false;
        for (const Pad& p : g_pads)
            if (p.userId == user) known = true;
        if (known) continue;
        for (int i = 1; i < kMaxPads; ++i) {
            if (g_pads[i].handle < 0) {
                openPad(g_pads[i], user);
                break;
            }
        }
    }
}

bool poll(ControllerState& out) { return pollPad(0, out); }

bool pollPad(int index, ControllerState& out) {
    out = {};
    if (index < 0 || index >= kMaxPads || g_pads[index].handle < 0) return false;
    Pad& slot = g_pads[index];
    applyRumble(slot);
    applyLight(slot);
    OrbisPadData pad{};
    int rc = scePadReadState(slot.handle, &pad);
    slot.connected = rc == 0 && pad.connected;
    if (!slot.connected) return false;
    out.connected = true;
    uint32_t b = pad.buttons;
    out.dpadUp = (b & ORBIS_PAD_BUTTON_UP) != 0;
    out.dpadRight = (b & ORBIS_PAD_BUTTON_RIGHT) != 0;
    out.dpadDown = (b & ORBIS_PAD_BUTTON_DOWN) != 0;
    out.dpadLeft = (b & ORBIS_PAD_BUTTON_LEFT) != 0;
    out.btnA = (b & ORBIS_PAD_BUTTON_CROSS) != 0;
    out.btnB = (b & ORBIS_PAD_BUTTON_CIRCLE) != 0;
    out.btnX = (b & ORBIS_PAD_BUTTON_SQUARE) != 0;
    out.btnY = (b & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
    if (g_circle) std::swap(out.btnA, out.btnB);
    out.btnL1 = (b & ORBIS_PAD_BUTTON_L1) != 0;
    out.btnR1 = (b & ORBIS_PAD_BUTTON_R1) != 0;
    out.btnL3 = (b & ORBIS_PAD_BUTTON_L3) != 0;
    out.btnR3 = (b & ORBIS_PAD_BUTTON_R3) != 0;
    // DS4 → Xbox, per posizione: Options=Menu, Share=View, PS=Guide.
    // Il clic del touchpad non è View: apre il menu dell'app (btnAppMenu).
    // Il bit Share (0x1) c'è nel SDK Sony; l'enum OpenOrbis parte da L3.
    constexpr uint32_t kShare = 0x00000001;
    constexpr uint32_t kPlayStation = 0x00010000;
    out.btnOptions = (b & ORBIS_PAD_BUTTON_OPTIONS) != 0;
    out.btnTouchpad = (b & kShare) != 0;
    out.btnNexus = (b & kPlayStation) != 0;
    out.btnAppMenu = (b & ORBIS_PAD_BUTTON_TOUCH_PAD) != 0;
    out.rawLeftX = out.leftStickX = normStick(pad.leftStick.x);
    out.rawLeftY = out.leftStickY = normStick(pad.leftStick.y);
    out.rawRightX = out.rightStickX = normStick(pad.rightStick.x);
    out.rawRightY = out.rightStickY = normStick(pad.rightStick.y);
    radialDeadzone(out.leftStickX, out.leftStickY, g_deadzone[0].load());
    radialDeadzone(out.rightStickX, out.rightStickY, g_deadzone[1].load());
    out.triggerL2 = normTrigger(pad.analogButtons.l2);
    out.triggerR2 = normTrigger(pad.analogButtons.r2);
    if (pad.touch.fingers > 0 && slot.touchW && slot.touchH) {
        out.touching = true;
        out.touchX = std::clamp(pad.touch.touch[0].x / static_cast<float>(slot.touchW - 1), 0.0f, 1.0f);
        out.touchY = std::clamp(pad.touch.touch[0].y / static_cast<float>(slot.touchH - 1), 0.0f, 1.0f);
    }
    return true;
}

bool padConnected(int index) { return index >= 0 && index < kMaxPads && g_pads[index].connected.load(); }
std::string padUserName(int index) {
    if (index < 0 || index >= kMaxPads || g_pads[index].handle < 0) return {};
    return g_pads[index].name;
}
int32_t padUserId(int index) {
    if (index < 0 || index >= kMaxPads) return ORBIS_USER_SERVICE_USER_ID_INVALID;
    return g_pads[index].userId;
}

void setRumble(uint8_t large, uint8_t small, uint32_t durationMs, int pad) {
    if (pad < 0 || pad >= kMaxPads) return;
    g_pads[pad].rumbleUntil = durationMs ? platform::nowMs() + durationMs : 0;
    g_pads[pad].rumble = (uint32_t(large) << 8) | small;
}
void setTriggerRumble(uint8_t, uint8_t, uint32_t, int) {}
void setDeadzone(float left, float right) {
    g_deadzone[0] = left;
    g_deadzone[1] = right;
}
void setCircleConfirms(bool on) { g_circle = on; }
bool circleConfirms() { return g_circle.load(); }
void setTriggerFeel(int, int, int, bool) {}
void setTriggerLogging(bool) {}
void setTriggerResistanceActive(bool) {}
void setLightBar(uint8_t r, uint8_t g, uint8_t b, int pad) {
    if (pad < 0 || pad >= kMaxPads) return;
    g_pads[pad].light = 0x01000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}
void resetLightBar(int pad) {
    if (pad < 0 || pad >= kMaxPads) return;
    g_pads[pad].light = 0;
}
void setEmbeddedMicActive(bool, int) {}

}  // namespace xc::input
