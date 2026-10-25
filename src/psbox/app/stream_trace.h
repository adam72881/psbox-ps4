// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostica stream stile AJ-GFN: last_stage fsync + CSV present per frame.
// Non cambia la qualità: serve a vedere se si muore su decode/present/rete.
#pragma once

#include <cstdint>

namespace xc::app::trace {

// Apre/resetta i file in dataDir (last_stage.txt, trace_stream.csv).
void beginSession();
void endSession();

// Marca a disco (write+fsync). Sovrascrive last_stage.txt.
void setStage(const char* stage);

// Una riga CSV: t_ms,frame,src_w,src_h,dst_w,dst_h,scaled,pres_us,budget_us,over,drawn
// Scrive ogni frame per i primi 120, poi 1/30; sempre se over o !drawn.
void notePresent(uint64_t frame, int srcW, int srcH, int dstW, int dstH, uint64_t presentUs,
                 bool drawn);

// Riepilogo per secondo (log + riga in trace_stream.txt).
void tickSecond(uint64_t presented, uint64_t late, uint64_t decodeFail, uint64_t rtpPkts);

}  // namespace xc::app::trace
