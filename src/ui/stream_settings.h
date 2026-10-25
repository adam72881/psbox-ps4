/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../streaming/stream_profile.h"
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
#include <stdbool.h>
#include <stdint.h>
enum {
    X4_SETTINGS_OPEN = 1u, X4_SETTINGS_UP = 2u, X4_SETTINGS_DOWN = 4u,
    X4_SETTINGS_LEFT = 8u, X4_SETTINGS_RIGHT = 16u,
    X4_SETTINGS_CONFIRM = 32u, X4_SETTINGS_CANCEL = 64u
};
typedef struct {
    bool open;
    bool applied;
    unsigned char interaction_modes;
    unsigned row;
    X4StreamRequestProfile profile;
    X4StreamColorPreference color;
    X4StreamCpuVariant cpu;
    X4StreamDecoderDepth depth;
} X4StreamSettingsUi;
/* Main thread only. Opening, navigation and confirmation consume the entire
 * catalog frame. Draft changes become global settings only on a separate X.
 * Losing edit permission cancels the draft; it cannot alter an active or
 * pending launch/auth operation. */
bool x4_stream_settings_ui_handle(X4StreamSettingsUi *, unsigned actions, bool can_edit);
void x4_stream_settings_ui_draw(const X4StreamSettingsUi *, uint32_t *pixels);
void x4_stream_settings_summary(char *out, unsigned capacity);
/* Last confirmed selection only; opening/cancelling a draft never claims an
 * application. The real catalog draw uses this after the one-X commit. */
int x4_stream_settings_applied_text(const X4StreamSettingsUi *, char *out, unsigned capacity);
/* Bounded received-data banner, never inferred from the requested profile. */
int x4_stream_received_text(char *out, unsigned capacity, unsigned width,
    unsigned height, unsigned matrix_received, unsigned matrix_selected, bool full_range);
void x4_stream_received_draw(uint32_t *pixels, unsigned width, unsigned height,
    unsigned matrix_received, unsigned matrix_selected, bool full_range);
#endif
