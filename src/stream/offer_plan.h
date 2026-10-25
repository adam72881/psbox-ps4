/* SPDX-License-Identifier: GPL-3.0-or-later
 * Offerta SDP come XCloud4: solo H.264 PT 102 Constrained Baseline,
 * nessun transport-cc, audio RecvOnly senza SSRC mittente, REMB sì.
 */
#ifndef PSBOX_OFFER_PLAN_H
#define PSBOX_OFFER_PLAN_H

#include <string.h>

static const int kOfferVideoPt = 102;
static const int kOfferAudioRecvOnly = 1;
static const int kOfferTwcc = 0;
static const int kOfferExtraH264Pt = 0; /* niente 104/106 */
static const int kKeepaliveIntervalMs = 30000;

/* 1 se l'SDP locale rispetta la politica 01.54. */
static int offerSdpOk(const char* sdp) {
    if (!sdp || !sdp[0]) return 0;
    if (strstr(sdp, "transport-cc")) return 0;
    if (strstr(sdp, "draft-holmer-rmcat-transport-wide-cc")) return 0;
    if (!strstr(sdp, "a=rtpmap:102 H264") && !strstr(sdp, "a=rtpmap:102 H264/90000")) return 0;
    if (strstr(sdp, "a=rtpmap:104 ")) return 0;
    if (strstr(sdp, "a=rtpmap:106 ")) return 0;
    if (!strstr(sdp, "goog-remb")) return 0;
    /* Audio: recvonly (o inactive), non sendrecv con SSRC nostro. */
    if (strstr(sdp, "m=audio")) {
        const char* a = strstr(sdp, "m=audio");
        const char* next = strstr(a + 1, "\nm=");
        size_t n = next ? (size_t)(next - a) : strlen(a);
        char buf[2048];
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, a, n);
        buf[n] = 0;
        if (strstr(buf, "a=sendrecv") || strstr(buf, "a=sendonly")) return 0;
        if (!strstr(buf, "a=recvonly") && !strstr(buf, "a=inactive")) return 0;
    }
    return 1;
}

#endif
