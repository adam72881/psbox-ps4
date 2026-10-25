/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../input/controller.h"
#include "../input/touch_menu.h"
#include "../video/h264_demo.h"
#include "../audio/demo_audio.h"
#include "../auth/device_auth.h"
#include "../auth/xbox_catalog.h"
#include "../auth/xbox_session.h"
#include "../media/live_media.h"
#include "catalog_browser.h"
typedef struct { int page; int selected; int exit_requested; int exit_error; } X4Screen;
void x4_screen_update(X4Screen *screen, uint32_t pressed, uint32_t held);
void x4_screen_draw(const X4Screen *screen, const X4Controller *controller, uint32_t *pixels);
void x4_media_draw(const X4DemoVideo *video, const X4DemoAudio *audio, uint32_t *pixels);
void x4_exit_error_draw(int error, uint32_t *pixels);
void x4_auth_draw(const X4AuthSnapshot *auth, int busy, int closing, uint32_t *pixels);
void x4_session_draw(const X4SessionSnapshot *session, int busy, int closing, uint32_t *pixels);
void x4_live_status_draw(const X4LiveMediaSnapshot *media, int error, uint32_t *pixels);
void x4_touch_menu_draw(const X4TouchMenu *menu, int muted, uint32_t *pixels);
void x4_live_overlay(const X4LiveMediaSnapshot *media, const X4SessionSnapshot *session, int muted,
                     const X4TouchMenu *touch_menu, bool show_banner, uint32_t *pixels);
bool x4_auth_qr_payload(const char *uri, const char *code, char *payload_out, size_t cap);
