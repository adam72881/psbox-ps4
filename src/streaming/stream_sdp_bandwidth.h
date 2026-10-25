/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdbool.h>
typedef struct { unsigned video_sections, as_lines, first_as_kbps, malformed_as_lines; } X4SdpVideoBandwidth;
/* Only media-level video b=AS changes. Every audio/application/session line
 * and every secret/candidate byte is preserved without inspecting its value.
 * Caller owns a NUL-terminated bounded buffer. Failure leaves it unchanged. */
int x4_stream_sdp_limit_video(char *sdp,size_t capacity,unsigned kbps);
bool x4_stream_sdp_video_bandwidth(const char *,size_t length,X4SdpVideoBandwidth *);
