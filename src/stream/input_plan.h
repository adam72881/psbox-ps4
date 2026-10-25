/* SPDX-License-Identifier: GPL-3.0-or-later
 * Politica canale input come XCloud4 rtc_transport.c: SCTP unordered +
 * maxRetransmits=0, ClientMetadata all'apertura, poi solo gamepad a 60 Hz.
 * Nessun Metadata type1 (frame-ack) di default: un pacchetto reliable in coda
 * bloccava l'input e il server metteva in pausa lo stream (01.52).
 */
#ifndef PSBOX_INPUT_PLAN_H
#define PSBOX_INPUT_PLAN_H

/* ~60 Hz: intervallo 16 ms (XCloud4 thread pad). */
static const int kInputPadHz = 60;
static const int kInputPadIntervalMs = 16;

/* DataChannelInit reliability per il canale "input". */
static const int kInputUnordered = 1;
static const unsigned kInputMaxRetransmits = 0;

/* Frame-ack Metadata (type 1): off di default. Accendi solo con flag esplicito. */
static const int kSendFrameAcksDefault = 0;

/* Skip del send se la coda SCTP del canale input ha già byte in uscita. */
static int inputSkipIfBuffered(size_t bufferedAmount) {
    return bufferedAmount > 0 ? 1 : 0;
}

#endif
