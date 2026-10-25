# SPDX-License-Identifier: GPL-3.0-only
"""Run the real submit_display against real scanout ownership and native doubles.

Only the bounded submit function and its export statement are extracted. The
decoder, converters, workers and native VideoOut are not run by this fixture.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/video/stream_pipeline.c").read_text(encoding="utf8")
signature = "static int submit_display(X4StreamPipeline *p)"
begin = source.index(signature)
body = source.index("{", begin)
end, depth = body + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
submit = source[begin:end]
assert submit.count("sceVideoOutSubmitFlip(") == 1
assert "ORBIS_VIDEO_OUT_FLIP_HSYNC:ORBIS_VIDEO_OUT_FLIP_VSYNC" in submit
assert "p->request.interaction_modes&X4_INTERACTION_VSYNC_OFF" in submit
export_begin = source.index('    fprintf(f,",\\\"synchronization\\\":')
export_end = source.index(";", export_begin) + 1
export = source[export_begin:export_end]
# A semicolon inside the JSON scope is not a C statement boundary.
while not export.rstrip().endswith("ORBIS_VIDEO_OUT_FLIP_VSYNC);"):
    export_end = source.index(";", export_end) + 1
    export = source[export_begin:export_end]
assert "metrics.stages" in export
assert "p->request=x4_stream_request_capture(request);" in source

fixture = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "src/streaming/stream_profile.h"
#include "src/video/video_pipeline.h"
#include "src/video/stream_diagnostics.h"
#include <orbis/VideoOut.h>
_Static_assert(ORBIS_VIDEO_OUT_FLIP_VSYNC==1&&ORBIS_VIDEO_OUT_FLIP_HSYNC==2,
    "host constants corroborated against installed SDK video.h:13-14");
typedef struct {
    X4StreamRequestConfig request;
    X4VideoPipelineDisplay display;
    int display_handle;
} X4StreamPipeline;
static uint64_t next_stream_flip=UINT64_C(1)<<61;
static uint64_t now=1000;
static unsigned native_calls,done_calls;
static int observed_mode,flip_rc,done_rc;
static int native_slot;
static uint64_t native_arg;
static uint64_t now_us(void) { return now+=10; }
static void duration(X4StreamPipeline *p,X4StreamStage stage,uint64_t us)
{ (void)p;assert(stage==X4_STREAM_FLIP_CALL||stage==X4_STREAM_CONVERT_TO_SUBMIT);assert(us<=now); }
int sceVideoOutSubmitFlip(int handle,int slot,int mode,int64_t arg)
{ assert(handle==7);++native_calls;observed_mode=mode;native_slot=slot;native_arg=(uint64_t)arg;return flip_rc; }
static int sceGnmSubmitDone(void) { ++done_calls;return done_rc; }
@SUBMIT@
static void export_mode(FILE *f,const X4StreamPipeline *p)
{ @EXPORT@ }
static X4VideoPipelineLease ready(X4StreamPipeline *p,uint64_t id)
{
    X4VideoPipelineLease lease;
    assert(!x4_video_pipeline_acquire_scanout(&p->display,&lease));
    assert(!x4_video_pipeline_finish_scanout(&p->display,lease,id,now-100,now-50));
    return lease;
}
int main(void)
{
    for(unsigned modes=0;modes<=X4_INTERACTION_MASK;++modes) {
        assert(x4_stream_settings_set_extended(X4_REQUEST_1080P_G8,X4_COLOR_AUTO_709,
            X4_CPU_0,X4_DEPTH_AUTO,modes));
        X4StreamPipeline p={.request=x4_stream_request_capture(NULL),.display_handle=7};
        assert(!x4_video_pipeline_display_init(&p.display,1));
        /* A later menu choice cannot mutate this session's flip policy. */
        assert(x4_stream_settings_set_extended(X4_REQUEST_720P_HQ,X4_COLOR_FORCE_601,
            X4_CPU_3,X4_DEPTH_1,modes^X4_INTERACTION_MASK));
        unsigned calls=native_calls,done=done_calls;
        assert(!submit_display(&p)&&native_calls==calls&&done_calls==done);
        X4VideoPipelineLease first=ready(&p,1),shown;
        assert(!submit_display(&p)&&native_calls==calls+1&&done_calls==done+1);
        assert(native_slot==(int)first.slot&&p.display.pending==(int)first.slot);
        assert(observed_mode==((modes&X4_INTERACTION_VSYNC_OFF)?ORBIS_VIDEO_OUT_FLIP_HSYNC:ORBIS_VIDEO_OUT_FLIP_VSYNC));
        assert(!submit_display(&p)&&native_calls==calls+1); /* pending is never resubmitted */
        X4VideoPipelineLease blocked;
        assert(x4_video_pipeline_acquire_scanout(&p.display,&blocked)==X4_PIPELINE_WAIT);
        assert(!x4_video_pipeline_observe_flip(&p.display,native_arg,now+100,&shown));
        now+=100;
        assert(shown.slot==first.slot&&p.display.screen==(int)first.slot);
        X4VideoPipelineLease second=ready(&p,2);
        assert(second.slot!=first.slot); /* native SCREEN is not overwritten */
        assert(!submit_display(&p)&&native_calls==calls+2);
        assert(!x4_video_pipeline_observe_flip(&p.display,native_arg,now+100,&shown));
        now+=100;
        assert(p.display.slots[first.slot].state==X4_SCANOUT_FREE);
        assert(!x4_video_pipeline_display_closed(&p.display));
        FILE *f=tmpfile();assert(f);export_mode(f,&p);assert(!fflush(f));rewind(f);
        char json[1024]={0};assert(fread(json,1,sizeof(json)-1,f)>0);assert(!fclose(f));
        assert(strstr(json,(modes&X4_INTERACTION_VSYNC_OFF)?"\"vsync\":false":"\"vsync\":true"));
        assert(strstr(json,(modes&X4_INTERACTION_VSYNC_OFF)?"\"native_flip_mode\":2":"\"native_flip_mode\":1"));
        assert(strstr(json,"decode_end_to_flip_observation")&&strstr(json,"submit_return_to_flip_observation"));
    }
    X4StreamPipeline p={.display_handle=7};assert(!x4_video_pipeline_display_init(&p.display,2));
    X4VideoPipelineLease lease=ready(&p,3);unsigned done=done_calls;
    flip_rc=-55;assert(submit_display(&p)==-55&&p.display.pending<0&&done_calls==done);
    assert(p.display.slots[lease.slot].state==X4_SCANOUT_READY);
    flip_rc=0;done_rc=-66;assert(submit_display(&p)==-66&&p.display.pending==(int)lease.slot);
    assert(!x4_video_pipeline_display_closed(&p.display));
    puts("PASS real submit_display/export: VSYNC and HSYNC, 16 independent bit combinations, immutable capture, one frame in flight and native failures");
    return 0;
}
'''.replace("@SUBMIT@", submit).replace("@EXPORT@", export)
assert not re.search(r"@[A-Z_]+@", fixture)
with tempfile.TemporaryDirectory(prefix="x4-stream-vsync-") as directory:
    out = Path(directory)
    unit = out / "vsync.c"
    unit.write_text(fixture, encoding="utf8")
    command = shlex.split(os.environ.get("CC", "gcc")) + [
        "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-DX4_STREAM_PROFILE=1",
        "-DX4_VIDEO_PROBE=0", "-I" + str(root),
        "-I" + str(root / "tests/video_capacity_stubs"),
        "-I" + str(root / "tests/host_stubs"), str(unit),
        str(root / "src/video/video_pipeline.c"),
        str(root / "src/streaming/stream_profile.c"), "-Wl,--gc-sections",
    ] + shlex.split(os.environ.get("X4_TEST_CFLAGS", "")) + ["-o", str(out / "vsync")]
    subprocess.run(command, check=True, cwd=root)
    subprocess.run([str(out / "vsync")], check=True, cwd=root)
