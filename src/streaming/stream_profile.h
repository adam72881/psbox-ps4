/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifndef X4_STREAM_PROFILE
#define X4_STREAM_PROFILE 0
#endif
#ifndef X4_PUBLIC
#define X4_PUBLIC 0
#endif

/* The compile flag still protects the experimental pipeline. Request settings
 * are process-local and captured by value before each session is started. */
typedef enum X4StreamRequestProfile {
    X4_REQUEST_720P60 = 0, X4_REQUEST_1080P_G = 1,
    X4_REQUEST_1080P_H = 2, X4_REQUEST_1080P_HQ = 3,
    X4_REQUEST_720P_HQ = 4, X4_REQUEST_1080P_G8 = 5,
    X4_REQUEST_PROFILE_MAX = X4_REQUEST_1080P_G8,
    X4_REQUEST_PROFILE_COUNT = 6,
    /* Internal source compatibility: A/B retain their numeric IDs. Protected
     * video/media/RTC use C as an admission ceiling, extended through ID5. */
    X4_REQUEST_1080P_A = X4_REQUEST_1080P_G,
    X4_REQUEST_1080P_B = X4_REQUEST_1080P_H,
    /* C is used only as the protected admission ceiling. It deliberately
     * follows MAX after appending G8; 1080p-HQ keeps its own ID 3. */
    X4_REQUEST_1080P_C = X4_REQUEST_PROFILE_MAX
} X4StreamRequestProfile;
typedef enum X4StreamColorPreference {
    X4_COLOR_AUTO_709 = 0, X4_COLOR_FORCE_601 = 1
} X4StreamColorPreference;
typedef enum X4StreamCpuVariant {
    X4_CPU_0 = 0, X4_CPU_1 = 1, X4_CPU_2 = 2, X4_CPU_3 = 3
} X4StreamCpuVariant;
typedef enum X4StreamDecoderDepth {
    X4_DEPTH_AUTO = 0, X4_DEPTH_1 = 1, X4_DEPTH_2 = 2
} X4StreamDecoderDepth;
/* Local session options; packed into settings bits8..11, independently of
 * profile/color/CPU/depth. Zero selects the defaults, including VSYNC. */
enum {
    X4_INTERACTION_INPUT_60HZ = 1u,
    X4_INTERACTION_RUMBLE_OFF = 2u,
    X4_INTERACTION_AUDIO_PREVIOUS = 4u,
    X4_INTERACTION_VSYNC_OFF = 8u,
    X4_INTERACTION_MASK = 15u
};
typedef struct X4StreamRequestConfig {
    X4StreamRequestProfile profile;
    X4StreamColorPreference color_preference;
    unsigned width, height, fps, bitrate_kbps;
    const char *os_name, *label, *h264_fmtp;
    bool display_info;
    /* Occupies existing padding: keep the captured config's 64-byte ABI. */
    unsigned char cpu_variant;
    unsigned char decoder_depth;
    unsigned char interaction_modes;
    unsigned h264_profile, max_fs, max_mbps;
} X4StreamRequestConfig;

/* Invalid enum arguments produce the safe 720p/automatic defaults. No string
 * supplied by a caller is retained; all returned pointers are fixed literals. */
X4StreamRequestConfig x4_stream_request_config(X4StreamRequestProfile, X4StreamColorPreference);
X4StreamRequestConfig x4_stream_settings_get(void);
bool x4_stream_settings_set(X4StreamRequestProfile, X4StreamColorPreference);
bool x4_stream_settings_set_cpu(X4StreamRequestProfile, X4StreamColorPreference, X4StreamCpuVariant);
bool x4_stream_settings_set_all(X4StreamRequestProfile, X4StreamColorPreference,
    X4StreamCpuVariant, X4StreamDecoderDepth);
bool x4_stream_settings_set_extended(X4StreamRequestProfile, X4StreamColorPreference,
    X4StreamCpuVariant, X4StreamDecoderDepth, unsigned interaction_modes);
X4StreamRequestConfig x4_stream_request_capture(const X4StreamRequestConfig *);
#if X4_STREAM_PROFILE
#include <stdio.h>
/* Owner resets before a new session after the previous sender has joined;
 * final report writing occurs after the current sender has joined. */
void x4_rtc_input_metrics_reset(void);
void x4_rtc_input_metrics_write(FILE *);
void x4_main_interaction_metrics_write(FILE *);
void x4_live_interaction_metrics_write(FILE *);
#endif
/* Local decoder policy only: never added to /play, headers, aliases or SDP.
 * Auto follows the received geometry on every decoder recreation. With no
 * SPS yet, the captured request's canonical dimensions are the initial hint. */
unsigned x4_stream_request_resolve_depth(const X4StreamRequestConfig *, unsigned width, unsigned height);
/* The verified native 720p header is invariant on titles, MRU and every
 * session endpoint except /play, including SDP/ICE/keepalive/DELETE. The
 * protocol clientAppVersion intentionally remains the baseline value. */
#define X4_STREAM_NATIVE_720_DEVICE_INFO \
    "{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\",\"clientAppType\":\"native\"," \
    "\"clientAppVersion\":\"0.7.33\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\"," \
    "\"model\":\"PS4\"},\"os\":{\"name\":\"android\",\"platform\":\"console\"}}}"
/* Public JSON only; these functions never accept credentials or arbitrary
 * strings except a bounded title ID using the existing ASCII ID grammar. */
/* This constructor is exclusively the /play header, never other endpoints. */
bool x4_stream_request_device_info(const X4StreamRequestConfig *, char *, size_t);
bool x4_stream_request_play_body(const X4StreamRequestConfig *, const char *, char *, size_t);
bool x4_stream_request_capabilities(const X4StreamRequestConfig *, char *, size_t);
bool x4_stream_request_dimensions(const X4StreamRequestConfig *, char *, size_t);
/* Stable enum-derived runtime control value; never an unvalidated string or
 * an extra field in the captured config. NULL means no alias message. */
const char *x4_stream_request_resolution_alias(const X4StreamRequestConfig *);
#if X4_STREAM_PROFILE < 0 || X4_STREAM_PROFILE > 2
#error X4_STREAM_PROFILE must be 0 (legacy), 1 (720p60), or 2 (1080p60)
#endif
#if X4_STREAM_PROFILE == 2
#define X4_STREAM_WIDTH 1920
#define X4_STREAM_HEIGHT 1080
#define X4_STREAM_FPS 60
#define X4_STREAM_BITRATE_KBPS 12000
#define X4_STREAM_OS "windows"
#define X4_STREAM_LABEL "1080p60"
/* RFC 6184 requires max-fs/max-mbps >= the signaled level's ceilings. */
#define X4_STREAM_H264_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240"
#elif X4_STREAM_PROFILE == 1
#define X4_STREAM_WIDTH 1280
#define X4_STREAM_HEIGHT 720
#define X4_STREAM_FPS 60
#define X4_STREAM_BITRATE_KBPS 10000
#define X4_STREAM_OS "android"
#define X4_STREAM_LABEL "720p60"
#define X4_STREAM_H264_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000"
#else
#define X4_STREAM_WIDTH 960
#define X4_STREAM_HEIGHT 540
#define X4_STREAM_FPS 30
#define X4_STREAM_BITRATE_KBPS 5000
#define X4_STREAM_OS "Orbis"
#define X4_STREAM_LABEL "legacy"
#define X4_STREAM_H264_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f;max-fs=3600;max-mbps=108000"
#endif
