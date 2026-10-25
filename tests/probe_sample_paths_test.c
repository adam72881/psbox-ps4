/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_case_catalog.h"
#include "../src/video/probe_contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    unsigned checks=0;X4VideoCapacity c;
    assert(x4_video_capacity_plan(0,&c)==0);
    for(unsigned i=0;i<X4_PROBE_ALL_CASE_COUNT;++i){
        const X4ProbeCase *p=x4_probe_case_at(i);assert(p);++checks;
        char path[128];
        const char *exts[]={"h264","x4au","rgba"};
        const char *paths[]={p->clip_path,p->index_path,p->reference_path};
        for(unsigned j=0;j<3;++j){snprintf(path,sizeof(path),"/data/xcloud4-probe/%s.%s",p->id,exts[j]);assert(!strcmp(paths[j],path));++checks;}
#if X4_VIDEO_CAPACITY_1080
        assert(x4_video_capacity_plan(1,&c)==0);
#endif
        int rc=x4_probe_case_validate(p,&c);
        assert(rc==(p->visible_width>c.visible_width?X4_PROBE_CONTRACT_GEOMETRY:0));++checks;
    }
    X4ProbeCase bad=*x4_probe_case_at(0);
    bad.id="../escape";assert(x4_probe_case_validate(&bad,&c)==X4_PROBE_CONTRACT_IDENTITY);++checks;
    bad=*x4_probe_case_at(0);bad.clip_path="/data/normal-app/control-720p30-cb.h264";
    assert(x4_probe_case_validate(&bad,&c)==X4_PROBE_CONTRACT_IDENTITY);++checks;
    bad=*x4_probe_case_at(0);bad.id="CONTROL-720p30-cb"; /* case-sensitive paths must still match */
    assert(x4_probe_case_validate(&bad,&c)==X4_PROBE_CONTRACT_IDENTITY);++checks;
    printf("PASS generated sample identities/paths checks=%u capacity=%d; no native support claim\n",checks,X4_VIDEO_CAPACITY_1080);
}
