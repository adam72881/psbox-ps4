/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/stream_diagnostics.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static void *receive(void *arg)
{ for(unsigned i=0;i<1000;++i)x4_stream_diagnostics_received(arg,100,1000100); return NULL; }
static void diagnostics_storage_contract(void)
{
    /* Preserve the old non-depth allowance. The only growth is exactly three
     * bounded Stage histograms, each with 2048 bins and four uint64 counters.
     * This is heap diagnostic storage, not the native-memory or stack limit. */
    const size_t base_bytes=326104;
    const size_t stage_bytes=(X4_STREAM_METRIC_BINS+4u)*sizeof(uint64_t);
    const size_t added_bytes=3u*stage_bytes;
    const size_t actual=x4_stream_diagnostics_storage_bytes();
    assert(X4_STREAM_STAGE_COUNT==13+3);
    assert(stage_bytes==16416 && added_bytes==49248);
    assert(actual==base_bytes+added_bytes);
    assert(actual-added_bytes<320u*1024u);
    printf("actual diagnostics storage: %zu bytes; base %zu + three stages %zu; old non-depth allowance 327680\n",
        actual,base_bytes,added_bytes);
}
static void new_diagnostics(const char *path)
{
    X4StreamDiagnostics *d=x4_stream_diagnostics_create(0);assert(d);
    diagnostics_storage_contract();
    x4_stream_diagnostics_keyframe_request(d,100);
    x4_stream_diagnostics_keyframe_request(d,300100);
    x4_stream_diagnostics_keyframe_request(d,600100);
    for(unsigned i=0;i<30;++i)x4_stream_diagnostics_au_received(d,false,1000+i);
    for(unsigned i=0;i<60;++i)x4_stream_diagnostics_au_received(d,false,1001000+i);
    x4_stream_diagnostics_au_received(d,true,2000100);
    x4_stream_diagnostics_keyframe_request(d,1900100); /* Simulated late publication across prior IDR. */
    x4_stream_diagnostics_au_received(d,true,2100100);
    for(unsigned i=0;i<38;++i)x4_stream_diagnostics_au_received(d,false,2101000+i);
    x4_stream_diagnostics_au_received(d,false,4001000);
    x4_stream_diagnostics_present(d,1000);
    x4_stream_diagnostics_present(d,201001); /* unknown */
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_RTP_GAP);
    x4_stream_diagnostics_present(d,402002);
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_WAIT_IDR|X4_STREAM_PAUSE_DECODER);
    x4_stream_diagnostics_present(d,603003); /* mixed, not asserted causality */
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_WAIT_IDR);
    x4_stream_diagnostics_present(d,804004);
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_DECODER);
    x4_stream_diagnostics_present(d,1005005);
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_PRESENTATION);
    x4_stream_diagnostics_present(d,1206006);
    /* Pending requests and over-capacity events remain explicit. */
    for(unsigned i=0;i<131;++i)x4_stream_diagnostics_keyframe_request(d,3000000+i);
    char name[1024];int n=snprintf(name,sizeof(name),"%s.new",path);assert(n>0&&(size_t)n<sizeof(name));
    FILE *f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,4500000));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
    d=x4_stream_diagnostics_create(0);assert(d);
    for(unsigned i=0;i<260;++i){x4_stream_diagnostics_evidence(d,i&15u);x4_stream_diagnostics_present(d,1000+(uint64_t)i*200001);}
    n=snprintf(name,sizeof(name),"%s.retention",path);assert(n>0&&(size_t)n<sizeof(name));
    f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,53000000));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
    d=x4_stream_diagnostics_create(0);assert(d);
    for(unsigned i=0;i<65536;++i)x4_stream_diagnostics_au_received(d,false,1000);
    x4_stream_diagnostics_au_received(d,false,UINT64_C(3600000001));
    n=snprintf(name,sizeof(name),"%s.capacity",path);assert(n>0&&(size_t)n<sizeof(name));
    f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,UINT64_C(3601000000)));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
    d=x4_stream_diagnostics_create(0);assert(d);
    for(unsigned second=0;second<125;++second)
        for(unsigned i=0;i<1+second%3;++i)
            x4_stream_diagnostics_au_received(d,true,(uint64_t)second*1000000+1000+i*10000);
    x4_stream_diagnostics_au_received(d,true,125100000);
    n=snprintf(name,sizeof(name),"%s.idr-minute",path);assert(n>0&&(size_t)n<sizeof(name));
    f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,125500000));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
}
int main(int argc,char **argv)
{
    assert(argc==2); X4StreamDiagnostics *d=x4_stream_diagnostics_create(100);
    assert(d); diagnostics_storage_contract();
    for(unsigned i=0;i<100;++i){
        x4_stream_diagnostics_record(d,X4_STREAM_DECODE,1000+i*100);
        x4_stream_diagnostics_record(d,i&1?X4_STREAM_DECODE_IDR:X4_STREAM_DECODE_NON_IDR,1000+i*100);
    }
    x4_stream_diagnostics_record(d,X4_STREAM_CONVERSION,999999);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,receive,d));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    x4_stream_diagnostics_received(d,7,UINT64_C(3600000100));
    x4_stream_diagnostics_present(d,1000); x4_stream_diagnostics_present(d,101000);
    x4_stream_diagnostics_present(d,201001); /* strict >100 ms only */
    FILE *f=fopen(argv[1],"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,2000100));assert(!fclose(f));
    assert(x4_stream_diagnostics_presentations(d)==3 && x4_stream_diagnostics_first_present(d)==1000);
    x4_stream_diagnostics_free(d);new_diagnostics(argv[1]);puts("PASS cumulative metrics, AU cadence, pending/overflow keyframe-IDR timing and bounded unknown/mixed pause evidence");return 0;
}
