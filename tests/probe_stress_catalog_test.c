/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_case_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL %u %s\n",__LINE__,#x);exit(1);} } while(0)
int main(void)
{
    CHECK(X4_PROBE_CASE_COUNT==5 && X4_PROBE_STRESS_CASE_COUNT==3 &&
        X4_PROBE_ALL_CASE_COUNT==8+X4_PROBE_PIPELINE_CASE_COUNT);
    const char *ids[]={"control-720p30-cb","720p60-cb","1080p60-cb","1080p60-main","1080p60-high","S720-CB","S1080-CB","S1080-HIGH"};
    for(unsigned i=0;i<8;++i)CHECK(!strcmp(x4_probe_case_at(i)->id,ids[i]));
    CHECK(!x4_probe_case_at(X4_PROBE_ALL_CASE_COUNT) && !x4_probe_case_at(UINT32_MAX));
    for(unsigned i=0;i<5;++i)CHECK(x4_probe_required_compatibility(i)==-1);
    CHECK(x4_probe_required_compatibility(5)==1 && x4_probe_required_compatibility(6)==2 && x4_probe_required_compatibility(7)==4);
    CHECK(x4_probe_required_compatibility(X4_PROBE_ALL_CASE_COUNT)==-2);
#if X4_VIDEO_PROBE
    CHECK(X4_PROBE_PIPELINE_CASE_COUNT==1 && !strcmp(x4_probe_case_at(8)->id,"1080p60-high-15M"));
    CHECK(x4_probe_case_pipeline_only(8) && x4_probe_required_compatibility(8)==-1);
#endif
    for(unsigned i=0;i<8;++i)CHECK(!x4_probe_case_pipeline_only(i));
    CHECK(x4_probe_required_compatibility(UINT32_MAX)==-2);
    const X4ProbeCase *stress=x4_probe_case_at(6);
    X4ProbeCase single600=*stress;single600.au_count=36000;single600.duration_us=600000000;
    CHECK(x4_probe_required_for(&single600,x4_probe_cases,X4_PROBE_CASE_COUNT)==2);
    X4ProbeCase reordered_stress[]={x4_probe_stress_cases[2],x4_probe_stress_cases[0],x4_probe_stress_cases[1]};
    const int expected[]={4,1,2};
    for(unsigned i=0;i<3;++i)CHECK(x4_probe_required_for(&reordered_stress[i],x4_probe_cases,X4_PROBE_CASE_COUNT)==expected[i]);
    X4ProbeCase reordered_compat[]={x4_probe_cases[4],x4_probe_cases[1],x4_probe_cases[0],x4_probe_cases[2],x4_probe_cases[3]};
    CHECK(x4_probe_required_for(stress,reordered_compat,5)==3);
    CHECK(!strcmp(reordered_compat[x4_probe_required_for(stress,reordered_compat,5)].id,"1080p60-cb"));
    for(unsigned i=0;i<3;++i){int prerequisite=x4_probe_required_for(&x4_probe_stress_cases[i],x4_probe_cases,5);
        const X4ProbeCase *c=&x4_probe_cases[prerequisite];const X4ProbeCase *s=&x4_probe_stress_cases[i];
        CHECK(c->profile_idc==s->profile_idc && c->level_idc==s->level_idc && c->visible_width==s->visible_width &&
            c->visible_height==s->visible_height && c->fps_num==s->fps_num && c->fps_den==s->fps_den);}
    X4ProbeCase invalid=*stress;invalid.id="S1080-UNKNOWN";CHECK(x4_probe_required_for(&invalid,x4_probe_cases,5)==-2);
    invalid=*stress;invalid.profile_idc=100;CHECK(x4_probe_required_for(&invalid,x4_probe_cases,5)==-2);
    invalid=*stress;invalid.level_idc=31;CHECK(x4_probe_required_for(&invalid,x4_probe_cases,5)==-2);
    invalid=*stress;invalid.visible_width=1280;CHECK(x4_probe_required_for(&invalid,x4_probe_cases,5)==-2);
    invalid=*stress;invalid.fps_num=30;CHECK(x4_probe_required_for(&invalid,x4_probe_cases,5)==-2);
    invalid=*stress;invalid.coded_height=1080;CHECK(x4_probe_required_for(&invalid,x4_probe_cases,5)==-2);
    invalid=*stress;invalid.crop_bottom=0;CHECK(x4_probe_required_for(&invalid,x4_probe_cases,5)==-2);
    X4ProbeCase duplicate[]={x4_probe_cases[2],x4_probe_cases[2]};CHECK(x4_probe_required_for(stress,duplicate,2)==-2);
    CHECK(x4_probe_required_for(stress,x4_probe_cases,2)==-2);
    CHECK(x4_probe_required_for(NULL,x4_probe_cases,5)==-2 && x4_probe_required_for(stress,NULL,5)==-2);
    bool passed[5]={true,true,true,true,true};
    CHECK(!x4_probe_gate_allows(-2,passed,5) && !x4_probe_gate_allows(-3,passed,5));
    CHECK(x4_probe_gate_allows(-1,passed,5) && x4_probe_gate_allows(2,passed,5));
    CHECK(!x4_probe_gate_allows(5,passed,5) && !x4_probe_gate_allows(2,NULL,5));
    CHECK(x4_probe_compatibility_begin(2,passed,5));
    CHECK(!passed[2] && passed[0] && passed[1] && passed[3] && passed[4]);
    CHECK(!x4_probe_gate_allows(2,passed,5)); /* A failing rerun cannot retain the old approval. */
    CHECK(!x4_probe_compatibility_begin(6,passed,5) && !x4_probe_compatibility_begin(2,NULL,5));
    passed[2]=true;CHECK(x4_probe_gate_allows(2,passed,5)); /* A new successful result may grant it. */
    X4ProbeStatus status=x4_probe_status_pending();const X4ProbeCase *sample=x4_probe_case_at(1);
    CHECK(!x4_probe_compatibility_pass(sample,&status,1,0));
    status.ready=true;status.source_eof=true;status.finished=true;
    status.pipeline_result=status.query_result=status.create_result=status.decode_result=status.reference_result=0;
    status.flush_result=status.close_result=status.delete_decoder_result=status.release_queue_result=status.reference_close_result=0;
    status.layout_verdict=X4_PROBE_LAYOUT_VERIFIED;status.input_aus=status.native_outputs=sample->au_count;status.rgb_publications=1;
    CHECK(x4_probe_compatibility_pass(sample,&status,1,0));
    CHECK(!x4_probe_compatibility_pass(sample,&status,0,0) && !x4_probe_compatibility_pass(sample,&status,1,1));
    status.native_outputs--;CHECK(!x4_probe_compatibility_pass(sample,&status,1,0));status.native_outputs++;
    status.layout_verdict=X4_PROBE_LAYOUT_INDETERMINATE;CHECK(!x4_probe_compatibility_pass(sample,&status,1,0));
    status.layout_verdict=X4_PROBE_LAYOUT_VERIFIED;status.reference_close_result=-1;CHECK(!x4_probe_compatibility_pass(sample,&status,1,0));
    status.reference_close_result=0;status.decode_result=X4_PROBE_RC_NOT_RUN;CHECK(!x4_probe_compatibility_pass(sample,&status,1,0));
    CHECK(!x4_probe_compatibility_pass(NULL,&status,1,0) && !x4_probe_compatibility_pass(sample,NULL,1,0));
    printf("PASS stress catalog checks=%u; host policy only, no native compatibility grant\n",checks);return 0;
}
