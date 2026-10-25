/* SPDX-License-Identifier: GPL-3.0-only */
/* Host pthread execution of the real four-reader copy owner/helpers. Native
 * decoder/GPU coherence and PS4 scheduling are not exercised. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <time.h>
#include "../src/video/live_h264.c"
static unsigned checks;
#define CHECK(x) do {++checks;assert(x);}while(0)
uint64_t sceKernelGetProcessTime(void)
{ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
int sceKernelUsleep(unsigned us)
{ struct timespec t={us/1000000,(long)(us%1000000)*1000};return nanosleep(&t,NULL); }
int scePthreadJoin(OrbisPthread t,void **out){return pthread_join(t,out);}
void x4_trace_record(X4Trace*t,uint16_t e,uint16_t f,uint64_t a,uint64_t b)
{(void)t;(void)e;(void)f;(void)a;(void)b;}
int main(void)
{
    VideoState s={0}; X4LiveVideo v={.state=&s};
    const size_t n=1920u*1088u*3u/2u;
    uint8_t *source=aligned_alloc(64,n+64),*staging=aligned_alloc(64,n+64);
    CHECK(source&&staging);s.output[0].p=source;s.output[0].size=n;
    s.staging=staging;s.staging_size=n;s.parallel_enabled=true;
    for(unsigned i=0;i<COPY_HELPERS;++i){
        CopyHelper*h=&s.helpers[i];h->owner=&s;h->index=i;
        atomic_init(&h->stop,false);atomic_init(&h->submitted,0);atomic_init(&h->completed,0);
        CHECK(!pthread_create(&h->thread,NULL,copy_worker,h));h->running=true;
    }
    for(unsigned pass=0;pass<80;++pass){
        size_t bytes=n-(pass%16)*16;
        for(size_t i=0;i<bytes;++i)source[i]=(uint8_t)(i*73+pass*19);
        memset(staging,0xcd,n+64);
        CHECK(!copy_picture(&v,&s,source,bytes));CHECK(!memcmp(source,staging,bytes));
        for(size_t i=bytes;i<n+64;++i)CHECK(staging[i]==0xcd);
        CHECK(s.parallel_enabled);CHECK(!v.copy_check_mismatch);
    }
    CHECK(v.copy_parallel_calls==80);CHECK(v.copy_check_pass==1);
    CHECK(!stop_copy_helpers(&s));CHECK(copy_helper_count(&s)==0);
    /* A cancelled/closed pool never releases storage until all real host
     * helper joins have returned. A fresh pool has separate sequence state. */
    for(unsigned i=0;i<COPY_HELPERS;++i)CHECK(atomic_load(&s.helpers[i].completed)==s.copy_sequence);
    free(source);free(staging);
    printf("PASS actual copy owner + three host helpers checks=%u copies=80; no native coherence/performance claim\n",checks);
    return 0;
}
