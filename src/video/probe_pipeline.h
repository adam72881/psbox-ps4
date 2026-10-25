/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#if X4_VIDEO_PROBE
#include "probe_case.h"
#include "../input/controller.h"
typedef struct {
    X4ProbeStatus status;
    int report_result;
    bool stopped, retained, cancelled, exit_requested, compatibility_passed;
    uint64_t started_us, ended_us;
} X4ProbePipelineRunResult;
/* UI closes its VideoOut before calling. This local-only runner owns a separate
 * triple-buffer VideoOut and explicit-stack workers until joined. converters
 * is 0 (decode-only), 2 or 3. A retained result forbids reopening UI/exiting. */
X4ProbePipelineRunResult x4_probe_pipeline_run(const X4ProbeCase *, X4ProbeMode,
    unsigned converters, X4Controller *);
#endif
