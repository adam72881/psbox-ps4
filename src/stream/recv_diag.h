/* SPDX-License-Identifier: GPL-3.0-or-later
 * Contatori permanenti del percorso di ricezione (causa 1 vs 2 nello stallo).
 * Aggiornati dallo stack (juice/DTLS); loggati 1 Hz da stream_player.
 * Nessun token.
 */
#ifndef PSBOX_RECV_DIAG_H
#define PSBOX_RECV_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void psbox_rd_poll_recv(void);
void psbox_rd_dtls_incoming(size_t n);
void psbox_rd_dtls_push(int ok, size_t qsize);
void psbox_rd_dorecv(void);
void psbox_rd_push_enter(void);
void psbox_rd_push_leave(void);
void psbox_rd_consent_sent(void);
void psbox_rd_consent_recv(void);
void psbox_rd_rtp(uint32_t ssrc);

typedef struct PsboxRecvSnap {
    uint64_t poll_recv;
    uint64_t dtls_incoming;
    uint64_t dtls_push_ok;
    uint64_t dtls_push_drop;
    uint64_t dorecv;
    uint64_t consent_sent;
    uint64_t consent_recv;
    uint64_t rtp_a;
    uint64_t rtp_b;
    uint32_t rtp_ssrc_a;
    uint32_t rtp_ssrc_b;
    int inside_push;
    int queue_size;
} PsboxRecvSnap;

void psbox_rd_snapshot(PsboxRecvSnap* out);
void psbox_rd_reset(void);
/* 01.56: log 1 Hz su thread proprio (anche se il worker è in start()). */
void psbox_rd_start_logger(void);
void psbox_rd_stop_logger(void);

#ifdef __cplusplus
}
#endif
#endif
