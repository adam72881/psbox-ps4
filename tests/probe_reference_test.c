/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic RGBA comparison/ownership tests. The injected fixture digest is
 * intentionally not SHA-256; native crypto gets separate binding verification. */
#define _POSIX_C_SOURCE 200809L
#include "probe_reference.h"
#include "probe_files.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static unsigned checks;
static void check(bool value,const char *expression,unsigned line)
{ ++checks;if(!value) { fprintf(stderr,"FAIL %u: %s\n",line,expression);exit(1); } }
#define CHECK(x) check(!!(x),#x,__LINE__)

#if X4_VIDEO_PROBE
typedef struct { char dir[80],samples[112],path[128];uint8_t sha[32];unsigned w,h,count; } Fixture;
typedef struct { int result;unsigned calls; } HashContext;
static int fixture_hash(FILE *file,uint64_t bytes,uint8_t actual[32],const atomic_bool *cancel,void *context)
{
    HashContext *ctx=context;if(ctx) { ++ctx->calls;if(ctx->result) return ctx->result; }
    uint64_t value=UINT64_C(0xcbf29ce484222325),total=0;uint8_t block[4096];
    while(total<bytes) {
        if(cancel && atomic_load(cancel)) return X4_PROBE_CANCELLED;
        size_t want=bytes-total<sizeof(block)?(size_t)(bytes-total):sizeof(block);
        if(fread(block,1,want,file)!=want) return X4_PROBE_IO;
        for(size_t i=0;i<want;++i) { value^=block[i];value*=UINT64_C(0x100000001b3); }total+=want;
    }
    for(unsigned i=0;i<32;++i) actual[i]=(uint8_t)(value>>((i&7u)*8));
    return 0;
}
static uint32_t pixel(unsigned index,unsigned x,unsigned y)
{ return 0xff000000u|((60u+index*20u+y)%240u)<<16|((40u+index*20u+x)%240u)<<8|((20u+index*20u+x+y)%240u); }
static Fixture fixture(unsigned w,unsigned h,unsigned count,bool duplicate)
{
    Fixture f={.dir="/tmp/x4-probe-reference.XXXXXX",.w=w,.h=h,.count=count};
    CHECK(mkdtemp(f.dir));CHECK(!x4_probe_files_test_root(f.dir));
    CHECK(snprintf(f.samples,sizeof(f.samples),"%s/xcloud4-probe",f.dir)>0);CHECK(!mkdir(f.samples,0700));
    CHECK(snprintf(f.path,sizeof(f.path),"%s/known.rgba",f.samples)>0);
    FILE *file=fopen(f.path,"wb");CHECK(file);
    uint8_t *row=malloc((size_t)w*4);CHECK(row);
    for(unsigned frame=0;frame<count;++frame) for(unsigned y=0;y<h;++y) {
        for(unsigned x=0;x<w;++x) {
            uint32_t value=pixel(duplicate?0:frame,x,y);
            for(unsigned c=0;c<4;++c) row[(size_t)x*4+c]=(uint8_t)(value>>(c*8));
        }
        CHECK(fwrite(row,4,w,file)==w);
    }
    free(row);CHECK(!fclose(file));file=fopen(f.path,"rb");CHECK(file);
    CHECK(!fixture_hash(file,(uint64_t)w*h*4*count,f.sha,NULL,NULL));CHECK(!fclose(file));return f;
}
static void remove_fixture(Fixture *f) { CHECK(!unlink(f->path));CHECK(!rmdir(f->samples));CHECK(!rmdir(f->dir)); }
static int open_reference(X4ProbeReference *r,Fixture *f,const atomic_bool *cancel,HashContext *ctx)
{ return x4_probe_reference_open_verified(r,f->path,f->w,f->h,f->count,f->sha,fixture_hash,ctx,cancel); }
static void picture(uint32_t *rgba,unsigned w,unsigned h,unsigned frame,unsigned add)
{
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) rgba[(size_t)y*w+x]=pixel(frame,x,y)+add*0x010101u;
}
static void scores(void)
{
    Fixture f=fixture(4,2,3,false);X4ProbeReference r={0};HashContext hash={0};CHECK(!open_reference(&r,&f,NULL,&hash));
    CHECK(hash.calls==1 && r.stats.hash_verified && r.frame_bytes==32);
    CHECK(open_reference(&r,&f,NULL,&hash)==X4_PROBE_STATE);
    uint32_t rgba[8];picture(rgba,4,2,1,0);X4ProbeReferenceScore score;
    CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));
    CHECK(score.best_reference_index==1 && score.accepted_references==1 && !score.frame_identity_ambiguous);
    CHECK(score.sum_abs==0 && score.samples==24 && score.mean_abs==0 && score.max_channel_delta==0 && !score.alpha_failures);
    picture(rgba,4,2,1,3);CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));
    CHECK(score.accepted_references==1 && score.mean_abs==3 && score.max_channel_delta==3);
    picture(rgba,4,2,1,4);CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));
    CHECK(score.accepted_references==0 && score.mean_abs==4);
    picture(rgba,4,2,1,0);rgba[0]+=16;CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));
    CHECK(score.accepted_references==1 && score.max_channel_delta==16 && score.sum_abs==16);
    ++rgba[0];CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));CHECK(score.accepted_references==0 && score.max_channel_delta==17);
    picture(rgba,4,2,1,0);rgba[0]&=0x00ffffffu;CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));
    CHECK(score.accepted_references==0 && score.alpha_failures==1 && score.mean_abs==0);
    picture(rgba,4,2,1,0);
    for(unsigned i=0;i<8;++i) rgba[i]=0xff000000u|((rgba[i]&255u)<<16)|(rgba[i]&0x0000ff00u)|((rgba[i]>>16)&255u);
    CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));CHECK(!score.accepted_references);
    CHECK(x4_probe_reference_match(&r,rgba,7,4,2,&score)==X4_PROBE_SIZE);
    CHECK(x4_probe_reference_match(&r,rgba,8,2,4,&score)==X4_PROBE_SIZE);
    CHECK(x4_probe_reference_match(&r,NULL,8,4,2,&score)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_match(&r,rgba,8,4,2,NULL)==X4_PROBE_ARGUMENT);
    CHECK(r.stats.comparisons==7 && r.stats.references_read==21 && r.stats.pixels_compared==168);
    CHECK(!x4_probe_reference_close(&r) && !r.state && r.stats.hash_verified);CHECK(!x4_probe_reference_close(&r));remove_fixture(&f);
    f=fixture(4,2,2,true);r=(X4ProbeReference){0};CHECK(!open_reference(&r,&f,NULL,NULL));picture(rgba,4,2,0,0);
    CHECK(!x4_probe_reference_match(&r,rgba,8,4,2,&score));CHECK(score.accepted_references==2 && score.frame_identity_ambiguous);
    CHECK(!x4_probe_reference_close(&r));remove_fixture(&f);
}
static void bounds(void)
{
    Fixture f=fixture(2,2,8,false);X4ProbeReference r={0};CHECK(!open_reference(&r,&f,NULL,NULL));
    uint32_t rgba[4];picture(rgba,2,2,7,0);X4ProbeReferenceScore score;
    CHECK(!x4_probe_reference_match(&r,rgba,4,2,2,&score));CHECK(score.accepted_references==1 && score.best_reference_index==7);
    CHECK(!x4_probe_reference_close(&r));
    CHECK(x4_probe_reference_open_verified(&r,f.path,2,2,9,f.sha,fixture_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_open_verified(&r,f.path,2,2,0,f.sha,fixture_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_open_verified(&r,f.path,1921,1080,1,f.sha,fixture_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_open_verified(&r,f.path,1920,1081,1,f.sha,fixture_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_open_verified(&r,f.path,0,2,1,f.sha,fixture_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_open_verified(&r,f.path,2,2,1,f.sha,fixture_hash,NULL,NULL)==X4_PROBE_SIZE);
    CHECK(x4_probe_reference_open_verified(&r,f.path,2,2,8,NULL,fixture_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_open_verified(&r,f.path,2,2,8,f.sha,NULL,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_reference_open_verified(&r,"https://invalid.test/x",2,2,8,f.sha,fixture_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    uint8_t wrong[32];memcpy(wrong,f.sha,32);wrong[0]^=1;
    CHECK(x4_probe_reference_open_verified(&r,f.path,2,2,8,wrong,fixture_hash,NULL,NULL)==X4_PROBE_HASH && !r.state);
    CHECK(!truncate(f.path,127));CHECK(open_reference(&r,&f,NULL,NULL)==X4_PROBE_SIZE);remove_fixture(&f);
    f=fixture(1920,1080,1,false);r=(X4ProbeReference){0};CHECK(!open_reference(&r,&f,NULL,NULL));
    CHECK(r.frame_bytes==8294400 && r.count==1);CHECK(!x4_probe_reference_close(&r));remove_fixture(&f);
}
static void cancellation_and_damage(void)
{
    Fixture f=fixture(4,2,1,false);atomic_bool cancel;atomic_init(&cancel,true);X4ProbeReference r={0};HashContext hash={0};
    CHECK(open_reference(&r,&f,&cancel,&hash)==X4_PROBE_CANCELLED && !hash.calls && !r.state);
    atomic_store(&cancel,false);hash.result=X4_PROBE_CANCELLED;
    CHECK(open_reference(&r,&f,&cancel,&hash)==X4_PROBE_CANCELLED && !r.state && r.stats.cancel_observations==1);
    hash.result=X4_PROBE_IO;CHECK(open_reference(&r,&f,&cancel,&hash)==X4_PROBE_IO && !r.state);
    hash.result=0;CHECK(!open_reference(&r,&f,&cancel,&hash));atomic_store(&cancel,true);
    uint32_t rgba[8];picture(rgba,4,2,0,0);X4ProbeReferenceScore score;
    CHECK(x4_probe_reference_match(&r,rgba,8,4,2,&score)==X4_PROBE_CANCELLED && !r.stats.references_read);
    CHECK(!x4_probe_reference_close(&r));atomic_store(&cancel,false);
    CHECK(!open_reference(&r,&f,&cancel,&hash));CHECK(!truncate(f.path,0));
    CHECK(x4_probe_reference_match(&r,rgba,8,4,2,&score)==X4_PROBE_IO);CHECK(!x4_probe_reference_close(&r));remove_fixture(&f);
}
#else
static void disabled(void)
{
    X4ProbeReference reference={0};X4ProbeReferenceScore score;uint32_t rgba[4]={0};
    CHECK(x4_probe_reference_open_verified(&reference,"/absent",2,2,1,NULL,NULL,NULL,NULL)==X4_PROBE_DISABLED);
    CHECK(x4_probe_reference_match(&reference,rgba,4,2,2,&score)==X4_PROBE_DISABLED);
    CHECK(!x4_probe_reference_close(&reference));CHECK(x4_probe_reference_close(NULL)==X4_PROBE_ARGUMENT);
}
#endif
int main(void)
{
#if X4_VIDEO_PROBE
    scores();bounds();cancellation_and_damage();
#else
    disabled();
#endif
    printf("PASS probe reference flag=%u checks=%u; RGBA matching only, no native image/PTS/layout claim\n",(unsigned)X4_VIDEO_PROBE,checks);
    return 0;
}
