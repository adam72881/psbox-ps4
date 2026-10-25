/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual bounded metadata store; no SDP strings, credentials or transport. */
#include "../src/media/live_trace.c"
#include <assert.h>
#include <pthread.h>
static X4Trace *first;
static void *writer(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<50000;++i)x4_trace_sdp_profile_record(first,1,102,i&1?0x42e020:0x64002a);
    return NULL;
}
int main(void)
{
    first=calloc(1,sizeof(*first));X4Trace *second=calloc(1,sizeof(*second));assert(first&&second);
    for(unsigned a=0;a<2;++a)for(unsigned b=0;b<128;++b){atomic_init(&first->sdp_profile_levels[a][b],0);atomic_init(&second->sdp_profile_levels[a][b],0);}
    x4_trace_sdp_profile_record(NULL,1,102,0x42e020);assert(!x4_trace_sdp_profile_get(NULL,1,102));
    x4_trace_sdp_profile_record(first,0,102,0x42e01f);x4_trace_sdp_profile_record(first,1,102,0x42e020);
    assert(x4_trace_sdp_profile_get(first,0,102)==0x42e01f);
    for(unsigned n=0;n<10000;++n){/* Critical log saturation cannot erase these independent cells. */
        x4_trace_sdp_profile_record(first,2,102,0x42e020);x4_trace_sdp_profile_record(first,1,128,0x42e020);
        x4_trace_sdp_profile_record(first,1,102,0x1000000);x4_trace_sdp_profile_record(first,1,102,0);}
    assert(x4_trace_sdp_profile_get(first,1,102)==0x42e020);
    pthread_t id;assert(!pthread_create(&id,NULL,writer,NULL));
    for(unsigned i=0;i<50000;++i){uint32_t n=x4_trace_sdp_profile_get(first,1,102);assert(n==0x42e020||n==0x64002a);assert(!x4_trace_sdp_profile_get(second,1,102));}
    assert(!pthread_join(id,NULL));assert(!x4_trace_sdp_profile_get(first,2,102));assert(!x4_trace_sdp_profile_get(first,1,128));
    unsigned values[4];
    for(unsigned a=0;a<2;++a)for(unsigned b=0;b<4;++b)atomic_init(&first->sdp_video_bandwidth[a][b],0);
    x4_trace_sdp_bandwidth_record(first,0,1,1,8000,0);x4_trace_sdp_bandwidth_record(first,1,1,0,0,0);
    x4_trace_sdp_bandwidth_get(first,0,values);assert(values[0]==1&&values[1]==1&&values[2]==8000&&!values[3]);
    x4_trace_sdp_bandwidth_get(first,1,values);assert(values[0]==1&&!values[1]&&!values[2]&&!values[3]);
    x4_trace_sdp_bandwidth_record(first,2,99,99,99,99);x4_trace_sdp_bandwidth_get(first,2,values);
    assert(!values[0]&&!values[1]&&!values[2]&&!values[3]);
    free(first);free(second);puts("PASS actual SDP numeric metadata: bounded PT/scope, concurrent publication, independent sessions and no diagnostic-window dependency");
}
