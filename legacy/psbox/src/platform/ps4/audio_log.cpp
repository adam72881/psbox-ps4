// SPDX-License-Identifier: GPL-3.0-or-later
// Inoltra i log di audio_orbis.c (Moonlight-ps4) nel log di PSBox.
#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" void xc_audio_log(const char* level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    // videodec2_loader.c e nv12_blit.c usano gli stessi macro, ma non sono audio.
    bool video = std::strncmp(buf, "videodec2:", 10) == 0 || std::strncmp(buf, "nv12_blit:", 10) == 0;
    const char* line = buf;
    char wrapped[576];
    if (!video) {
        std::snprintf(wrapped, sizeof wrapped, "audio: %s", buf);
        line = wrapped;
    }
    if (level && level[0] == 'E') XC_LOGE("%s", line);
    else if (level && level[0] == 'W') XC_LOGW("%s", line);
    else XC_LOGI("%s", line);
}
