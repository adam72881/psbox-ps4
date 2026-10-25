/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/stream_sps.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Synthetic syntax fixtures are independent of the real libx264 fixtures.
 * They cover legal syntax encoders here do not emit (POC1 and absent VUI). */
typedef struct {uint8_t p[1024];unsigned n;} Writer;
typedef struct {
    unsigned profile,constraints,level,id,chroma,depth_l,depth_c,poc,refs,dpb,reorder;
    unsigned width,height,crop_l,crop_r,crop_t,crop_b,frame;
    unsigned vui,signal,description,matrix,full,restriction,scaling,hrd;
} Fixture;
static Fixture standard(void)
{
    return (Fixture){.profile=66,.constraints=0xe0,.level=32,.chroma=1,.depth_l=8,.depth_c=8,
        .refs=3,.dpb=4,.width=1280,.height=720,.frame=1,.vui=1,.signal=1,.description=1,
        .matrix=6,.restriction=1};
}
static void put(Writer *w,unsigned v,unsigned n)
{for(unsigned i=n;i;--i){unsigned at=w->n++;assert(at<sizeof(w->p)*8);w->p[at/8]|=((v>>(i-1))&1)<<(7-at%8);}}
static void golomb(Writer *w,unsigned v)
{unsigned n=0,x=++v;while(x>>=1)++n;put(w,0,n);put(w,v,n+1);}
static bool high(unsigned p)
{switch(p){case 100:case 110:case 122:case 244:case 44:case 83:case 86:case 118:case 128:case 138:case 139:case 134:case 135:return true;default:return false;}}
static size_t fixture(uint8_t *out,const Fixture *f)
{
    Writer w={0};put(&w,f->profile,8);put(&w,f->constraints,8);put(&w,f->level,8);golomb(&w,f->id);
    if(high(f->profile)){
        golomb(&w,f->chroma);if(f->chroma==3)put(&w,0,1);
        golomb(&w,f->depth_l-8);golomb(&w,f->depth_c-8);put(&w,0,1);put(&w,f->scaling,1);
        if(f->scaling)for(unsigned i=0;i<(f->chroma==3?12u:8u);++i){
            put(&w,1,1);for(unsigned j=0;j<(i<6?16u:64u);++j)golomb(&w,0);
        }
    }
    golomb(&w,0);golomb(&w,f->poc);
    if(f->poc==0)golomb(&w,0);
    if(f->poc==1){put(&w,0,1);golomb(&w,0);golomb(&w,1);golomb(&w,2);golomb(&w,1);golomb(&w,2);}
    golomb(&w,f->refs);put(&w,0,1);golomb(&w,f->width/16-1);
    golomb(&w,f->height/(16*(2-f->frame))-1);put(&w,f->frame,1);if(!f->frame)put(&w,1,1);put(&w,1,1);
    bool crop=f->crop_l||f->crop_r||f->crop_t||f->crop_b;put(&w,crop,1);
    if(crop){unsigned x=f->chroma==1||f->chroma==2?2:1,y=(f->chroma==1?2:1)*(2-f->frame);
        golomb(&w,f->crop_l/x);golomb(&w,f->crop_r/x);golomb(&w,f->crop_t/y);golomb(&w,f->crop_b/y);}
    put(&w,f->vui,1);
    if(f->vui){
        put(&w,0,2);put(&w,f->signal,1);
        if(f->signal){put(&w,5,3);put(&w,f->full,1);put(&w,f->description,1);if(f->description){put(&w,1,8);put(&w,1,8);put(&w,f->matrix,8);}}
        put(&w,0,1);put(&w,1,1);put(&w,1,32);put(&w,120,32);put(&w,1,1);
        put(&w,f->hrd,1);if(f->hrd){golomb(&w,0);put(&w,0,8);golomb(&w,100);golomb(&w,200);put(&w,1,1);put(&w,0,20);}
        put(&w,0,1);if(f->hrd)put(&w,0,1);put(&w,0,1);put(&w,f->restriction,1);
        if(f->restriction){put(&w,1,1);golomb(&w,0);golomb(&w,0);golomb(&w,16);golomb(&w,16);golomb(&w,f->reorder);golomb(&w,f->dpb);}
    }
    put(&w,1,1);while(w.n%8)put(&w,0,1);
    memcpy(out,"\0\0\0\1\x67",5);size_t n=5;unsigned zeros=0;
    for(unsigned i=0;i<w.n/8;++i){unsigned x=w.p[i];if(zeros>=2&&x<=3){out[n++]=3;zeros=0;}out[n++]=(uint8_t)x;zeros=x?0:zeros+1;}
    return n;
}
static X4StreamSps accepted(const Fixture *f)
{
    uint8_t nal[2048];X4StreamSps s={0};X4StreamSpsDiagnostics d={0};size_t n=fixture(nal,f);
    assert(!x4_stream_sps_inspect_diagnostic(nal,n,&s,&d));assert(d.syntax_complete);
    assert(d.stage==X4_SPS_STAGE_DONE&&d.valid_mask&X4_SPS_HAVE_TRAILING);
    assert(s.profile_idc==f->profile&&s.level_idc==f->level&&s.seq_parameter_set_id==f->id);
    assert(s.chroma_format_idc==f->chroma&&s.bit_depth_luma==f->depth_l&&s.bit_depth_chroma==f->depth_c);
    assert(s.coded_width==f->width&&s.coded_height==f->height&&s.visible_width==f->width-f->crop_l-f->crop_r&&s.visible_height==f->height-f->crop_t-f->crop_b);
    assert(s.pic_order_cnt_type==f->poc&&s.frame_mbs_only_flag==f->frame);
    return s;
}
static void rejected(const Fixture *f,int code,bool complete)
{
    uint8_t nal[2048];size_t n=fixture(nal,f);X4StreamSps s={0};X4StreamSpsDiagnostics d={0};
    assert(x4_stream_sps_inspect_diagnostic(nal,n,&s,&d)==code);assert(d.code==code&&d.syntax_complete==complete);
    assert(s.profile_idc==f->profile&&d.valid_mask&X4_SPS_HAVE_PROFILE);
}
int main(void)
{
    uint8_t nal[2048],au[8192];Fixture f=standard();X4StreamSps s=accepted(&f),t={0};
    size_t n=fixture(nal,&f);assert(!x4_stream_sps_parse(nal+4,n-4,&t)&&x4_stream_sps_compatible(&s,&t));
    for(size_t i=0;i<n;++i){X4StreamSps before=t;assert(x4_stream_sps_parse(nal,i,&t));assert(!memcmp(&before,&t,sizeof(t)));}
    /* Announced identity/metadata are retained before a later truncated read. */
    X4StreamSpsDiagnostics d={0};assert(x4_stream_sps_inspect_diagnostic(nal,8,&t,&d)==X4_SPS_TRUNCATED);
    assert(t.profile_idc==66&&t.level_idc==32&&(d.valid_mask&X4_SPS_HAVE_LEVEL)&&!d.syntax_complete&&d.reader_error);
    unsigned profiles[]={66,77,88,100,110,122,244,44,83,86,118,128,138,139,134,135,17};
    for(unsigned i=0;i<sizeof(profiles)/sizeof(*profiles);++i){f=standard();f.profile=profiles[i];f.constraints=0;accepted(&f);}
    for(unsigned poc=0;poc<3;++poc){f=standard();f.poc=poc;accepted(&f);}
    f=standard();f.profile=100;f.width=1920;f.height=1088;f.crop_b=8;f.level=42;f.scaling=1;f.hrd=1;accepted(&f);
    f=standard();f.width=640;f.height=480;accepted(&f); /* geometry becomes scalar warning */
    f=standard();f.width=1280;f.height=736;f.crop_l=2;f.crop_r=4;f.crop_t=2;f.crop_b=14;accepted(&f);
    f=standard();f.dpb=8;f.refs=7;f.reorder=6;accepted(&f); /* native decides level/DPB */
    f=standard();f.level=99;f.profile=17;f.restriction=0;t=accepted(&f);assert(t.dpb_source==3&&t.max_dec_frame_buffering==3);
    f=standard();f.vui=0;t=accepted(&f);assert(t.matrix_coefficients==2&&t.full_range==0&&t.dpb_source==2);
    f=standard();f.restriction=0;t=accepted(&f);assert(t.dpb_source==2);
    for(unsigned matrix=0;matrix<12;++matrix)for(unsigned full=0;full<2;++full){f=standard();f.matrix=matrix;f.full=full;t=accepted(&f);assert(t.matrix_coefficients==matrix&&t.full_range==full);}
    f=standard();f.profile=100;f.chroma=2;rejected(&f,X4_SPS_CHROMA,true);
    f=standard();f.profile=100;f.chroma=3;f.scaling=1;rejected(&f,X4_SPS_CHROMA,true);
    f=standard();f.profile=110;f.depth_l=10;f.depth_c=10;rejected(&f,X4_SPS_BIT_DEPTH,true);
    f=standard();f.frame=0;f.height=736;rejected(&f,X4_SPS_INTERLACE,true);
    f=standard();f.width=1936;rejected(&f,X4_SPS_SIZE,true);
    f=standard();f.height=1104;rejected(&f,X4_SPS_SIZE,true);
    f=standard();f.crop_l=1280;rejected(&f,X4_SPS_CROP,false);
    f=standard();f.constraints|=1;rejected(&f,X4_SPS_PROFILE,false);
    f=standard();f.id=32;rejected(&f,X4_SPS_PROFILE,false);
    f=standard();f.poc=3;rejected(&f,X4_SPS_SYNTAX,false);
    f=standard();n=fixture(nal,&f);s=accepted(&f);
    memcpy(au,nal,n);memcpy(au+n,"\0\0\1\x68\x80\0\0\0\1\x65\x88\x84",13);
    assert(!x4_stream_sps_check_au(au,n+13,&s));assert(!x4_stream_sps_parse(au,n+13,&t));
    memcpy(au+n+13,nal,n);assert(!x4_stream_sps_check_au(au,2*n+13,&s));
    f.profile=77;f.constraints=0;size_t m=fixture(au+n+13,&f);assert(x4_stream_sps_check_au(au,n+13+m,&s)==X4_SPS_CHANGED);
    /* A final 3-byte start code must not silently erase a truncated last NAL. */
    assert(x4_stream_sps_check_au((const uint8_t*)"\0\0\1",3,&s)==X4_SPS_TRUNCATED);
    uint8_t bad_ebsp[]={0x67,66,0,32,0,0,3,4};
    assert(x4_stream_sps_inspect_diagnostic(bad_ebsp,sizeof(bad_ebsp),&t,&d)!=0);
    unsigned rng=17;
    for(unsigned j=0;j<20000;++j){for(unsigned i=0;i<sizeof(nal);++i){rng=rng*1664525u+1013904223u;nal[i]=(uint8_t)(rng>>24);}x4_stream_sps_inspect_diagnostic(nal,j%sizeof(nal),&t,&d);x4_stream_sps_check_au(nal,j%sizeof(nal),&s);}
    puts("stream SPS tolerant admission, partial diagnostics, High-family, POC 0/1/2, VUI/HRD/scaling, crop, EBSP, AU changes, truncation and 20k bounded fuzz PASS");
    return 0;
}
