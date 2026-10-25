/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/streaming/stream_profile.h"

int main(void)
{
    X4StreamRequestConfig cfg = x4_stream_settings_get();
#if X4_PUBLIC
    /* Public build: locked to 1080p-G8, Depth Auto, VSync OFF */
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.color_preference == X4_COLOR_AUTO_709);
    assert(cfg.cpu_variant == X4_CPU_0);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_OFF);
    assert(cfg.width == 1920u);
    assert(cfg.height == 1080u);

    /* Verify immutable: calls to set* functions do not alter the public settings */
    x4_stream_settings_set(X4_REQUEST_720P60, X4_COLOR_FORCE_601);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.color_preference == X4_COLOR_AUTO_709);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_OFF);

    x4_stream_settings_set_cpu(X4_REQUEST_1080P_HQ, X4_COLOR_FORCE_601, X4_CPU_3);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_OFF);

    x4_stream_settings_set_all(X4_REQUEST_720P_HQ, X4_COLOR_FORCE_601, X4_CPU_2, X4_DEPTH_1);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_OFF);

    x4_stream_settings_set_extended(X4_REQUEST_720P60, X4_COLOR_FORCE_601, X4_CPU_1, X4_DEPTH_2, 0);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_OFF);

    puts("PASS test_stream_public_settings (X4_PUBLIC=1: 1080p-G8, Depth Auto, VSync OFF, immutable)");
#else
    /* Lab build: default 720p-HQ, Depth Auto, VSync active (interaction_modes == 0) */
    assert(cfg.profile == X4_REQUEST_720P_HQ);
    assert(cfg.color_preference == X4_COLOR_AUTO_709);
    assert(cfg.cpu_variant == X4_CPU_0);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == 0);

    /* Lab build updates when set is called */
    x4_stream_settings_set_extended(X4_REQUEST_1080P_G8, X4_COLOR_AUTO_709, X4_CPU_0, X4_DEPTH_2, X4_INTERACTION_VSYNC_OFF);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.decoder_depth == X4_DEPTH_2);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_OFF);

    puts("PASS test_stream_public_settings (X4_PUBLIC=0: default VSync ON (0), mutable)");
#endif
    return 0;
}
