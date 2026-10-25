/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { X4_STREAM_METRIC_BINS = 2048, X4_STREAM_METRIC_QUANTUM_US = 100,
    X4_STREAM_BITRATE_SECONDS = 3600, X4_STREAM_PAUSE_RECORDS = 256,
    X4_STREAM_KEYFRAME_REQUESTS = 128, X4_STREAM_IDR_LATENCY_BINS = 6001 };
enum { X4_STREAM_PAUSE_RTP_GAP = 1, X4_STREAM_PAUSE_WAIT_IDR = 2,
    X4_STREAM_PAUSE_DECODER = 4, X4_STREAM_PAUSE_PRESENTATION = 8 };
typedef enum {
    X4_STREAM_DECODE, X4_STREAM_CONVERSION, X4_STREAM_STRIPE0, X4_STREAM_STRIPE1,
    X4_STREAM_DECODE_TO_FLIP, X4_STREAM_CONVERT_TO_SUBMIT, X4_STREAM_FLIP_CALL,
    X4_STREAM_SUBMIT_TO_OBSERVATION, X4_STREAM_PRESENT_INTERVAL,
    X4_STREAM_DECODE_IDR, X4_STREAM_DECODE_NON_IDR,
    X4_STREAM_FLIP_END_TO_CONVERSION, X4_STREAM_CONTROLLER_READ_INTERVAL,
    X4_STREAM_DECODE_WITH_OUTPUT, X4_STREAM_DECODE_WITHOUT_OUTPUT,
    X4_STREAM_NATIVE_OUTPUT_INTERVAL,
    X4_STREAM_STAGE_COUNT
} X4StreamStage;
typedef struct X4StreamDiagnostics X4StreamDiagnostics;
/* One recording owner per stage; all recording owners must be joined before
 * export/free. Cumulative 100-us bins retain the whole session, not a rolling
 * tail. Last bin is overflow: its percentile has no finite upper bound. */
X4StreamDiagnostics *x4_stream_diagnostics_create(uint64_t started_us);
void x4_stream_diagnostics_free(X4StreamDiagnostics *);
void x4_stream_diagnostics_record(X4StreamDiagnostics *, X4StreamStage, uint64_t us);
/* Concurrent Track callbacks: fixed storage and atomics only. Bytes are valid
 * video RTP callback bytes (including RTP header), not IP/UDP wire bytes. */
void x4_stream_diagnostics_received(X4StreamDiagnostics *, size_t bytes, uint64_t now_us);
/* Complete AU assembly owner, before the recovery filter. This is not a
 * count of RTP packets, submitted AUs or native outputs. */
void x4_stream_diagnostics_au_received(X4StreamDiagnostics *, bool idr, uint64_t now_us);
/* One auth-grant observer producer; paired to the next complete IDR observed
 * by the assembly owner, never to a Decode output or a presumed remote send. */
void x4_stream_diagnostics_keyframe_request(X4StreamDiagnostics *, uint64_t now_us);
/* Event association within the observed presentation interval. Flags are
 * evidence, not proof of a unique causal explanation; unknown/mixed retained. */
void x4_stream_diagnostics_evidence(X4StreamDiagnostics *, unsigned flags);
void x4_stream_diagnostics_present(X4StreamDiagnostics *, uint64_t now_us);
/* Real export body used by the runtime and host tests; writes no identities,
 * credentials, AU/pixel contents or network destinations. */
int x4_stream_diagnostics_write(FILE *, const X4StreamDiagnostics *, uint64_t ended_us);
uint64_t x4_stream_diagnostics_presentations(const X4StreamDiagnostics *);
uint64_t x4_stream_diagnostics_first_present(const X4StreamDiagnostics *);
size_t x4_stream_diagnostics_storage_bytes(void);
