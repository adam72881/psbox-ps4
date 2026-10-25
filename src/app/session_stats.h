/* SPDX-License-Identifier: GPL-3.0-or-later
 * Diagnostica di una sessione di stream. I thread video/audio/RTP aggiornano
 * solo contatori. Il file JSON lo scrive un thread a priorità bassa.
 */
#ifndef PSBOX_SESSION_STATS_H
#define PSBOX_SESSION_STATS_H

#include <stdint.h>
#include <string>

namespace xc::app::stats {

static const int kStatsKeepFiles = 10;
static const int kStatsIntervalMs = 5000;
static const int kTimeBuckets = 12;

struct TimeHist {
    uint64_t n = 0;
    uint64_t sumUs = 0;
    uint64_t bucket[kTimeBuckets] = {};
};

struct BrPoint {
    uint32_t tMs = 0;
    uint32_t bps = 0;
};

struct SessionSnap {
    std::string started;
    uint64_t durationMs = 0;
    std::string resolution;
    uint32_t bitrateAskBps = 0;
    uint32_t bitrateNowBps = 0;
    BrPoint bitrate[24] = {};
    int bitrateN = 0;
    uint64_t framesReceived = 0, framesDecoded = 0, framesPresented = 0;
    uint64_t droppedQueue = 0, droppedIdr = 0, droppedError = 0, droppedLatest = 0;
    double fpsReceived = 0, fpsDecoded = 0, fpsPresented = 0;
    uint64_t packetsLost = 0, nacksSent = 0, nacksRecovered = 0;
    uint64_t keyframeSent = 0, keyframeBlocked = 0;
    uint32_t queueMax = 0;
    double queueAvg = 0;
    uint64_t decodeMeanUs = 0, decodeP95Us = 0;
    uint64_t presentMeanUs = 0, presentP95Us = 0;
    uint64_t audioUnderrun = 0, audioTrimFrames = 0;
    uint64_t menuSuspend = 0, menuResume = 0;
    std::string exitReason;
};

/* Bordi superiori dei bucket, in microsecondi. L'ultimo è aperto. */
uint64_t timeBucketEdge(int i);
void timeHistAdd(TimeHist& h, uint64_t us);
uint64_t timeHistMean(const TimeHist& h);
uint64_t timeHistP95(const TimeHist& h);

std::string sessionFileName(const char* ymd, const char* hms);
/* Tiene i più recenti. `names` è ordinato dal più vecchio. Ritorna quanti togliere. */
int statsPruneCount(int have, int keep);
std::string renderSessionJson(const SessionSnap& s);

/* intervalMs <= 0 usa 5 s. I thread di stream non entrano qui a scrivere. */
void statsBegin(const std::string& dir, const char* resolution, uint32_t askBps, int intervalMs);
void statsNoteQueue(uint32_t depth);
void statsNoteDrop(int reason, uint64_t n); /* 0 coda, 1 IDR, 2 errorFrame/decode, 3 latest */
void statsNoteDecoded(uint64_t us);
void statsNotePresented(uint64_t us);
void statsNoteKf(int sent); /* 1 inviata, 0 bloccata dal limiter */
void statsNoteNet(uint64_t lost, uint64_t nacks, uint64_t recovered);
void statsNoteBitrate(uint32_t bps);
void statsNoteAudio(uint64_t underrun, uint64_t trimFrames);
void statsNoteMenu(int suspend); /* 1 sospensione, 0 ripresa */
void statsNoteReceived(uint64_t n);
/* Imposta l'esito e sveglia il thread. Non scrive e non aspetta. */
void statsFinish(const char* exitReason);
/* Una scrittura, se il mutex è libero. La usa la chiusura del processo. */
void statsWriteFinal();

}  // namespace xc::app::stats

#endif
