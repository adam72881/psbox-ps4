/* SPDX-License-Identifier: GPL-3.0-or-later
 * Pixel chiesti in dimensionschanged / SDP. L'alias di sessione è 720HQ /
 * 1080HQ / 1440: un confronto == "720" mandava 1920x1080 (bug 01.51).
 */
#ifndef PSBOX_STREAM_DIMS_H
#define PSBOX_STREAM_DIMS_H

#include <string.h>

struct StreamDims {
    int w;
    int h;
};

/* Alias che inizia con 720 → 1280x720; 1440 → 2560x1440; altrimenti 1920x1080. */
static StreamDims streamDimsForAlias(const char* alias) {
    StreamDims d;
    d.w = 1920;
    d.h = 1080;
    if (!alias || !alias[0]) return d;
    if (strncmp(alias, "1440", 4) == 0) {
        d.w = 2560;
        d.h = 1440;
        return d;
    }
    if (strncmp(alias, "720", 3) == 0) {
        d.w = 1280;
        d.h = 720;
        return d;
    }
    /* 1080, 1080HQ, altro */
    return d;
}

/* Constrained Baseline come XCloud4: 42e020 a 720 (L3.2), 42e02a a 1080 (L4.2). */
static const char* streamH264Fmtp(const char* alias) {
    StreamDims d = streamDimsForAlias(alias);
    if (d.h <= 720)
        return "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000";
    if (d.h >= 1440)
        return "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240";
    return "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240";
}

#endif
