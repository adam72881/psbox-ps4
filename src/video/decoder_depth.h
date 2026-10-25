/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../streaming/stream_profile.h"
#if X4_STREAM_PROFILE
enum { X4_DECODER_DEPTH_INPUT_MAX=4, X4_DECODER_DEPTH_OUTPUT_MAX=5 };
/* Sole native-owner ledger. This deliberately does not infer picture/AU
 * identity: the installed output ABI has no returned PTS or attachedData.
 * Depth-two input retirement after three later Decode returns is a
 * conservative caller assumption, not a lifetime guarantee from this SDK. */
typedef struct {
    unsigned depth, input_count, output_count, next_input;
    uint64_t decode_completed, submitted_at[X4_DECODER_DEPTH_INPUT_MAX];
    bool input_live[X4_DECODER_DEPTH_INPUT_MAX];
    bool output_held[X4_DECODER_DEPTH_OUTPUT_MAX];
    bool drained, deleted;
} X4DecoderDepthLedger;
int x4_decoder_depth_init(X4DecoderDepthLedger *,unsigned);
int x4_decoder_depth_input_acquire(X4DecoderDepthLedger *,unsigned *);
void x4_decoder_depth_decode_returned(X4DecoderDepthLedger *);
int x4_decoder_depth_output_offer(X4DecoderDepthLedger *,unsigned);
int x4_decoder_depth_output_return(X4DecoderDepthLedger *,unsigned);
void x4_decoder_depth_flush_complete(X4DecoderDepthLedger *);
void x4_decoder_depth_delete_complete(X4DecoderDepthLedger *);
unsigned x4_decoder_depth_inputs_live(const X4DecoderDepthLedger *);
unsigned x4_decoder_depth_outputs_held(const X4DecoderDepthLedger *);
#endif
