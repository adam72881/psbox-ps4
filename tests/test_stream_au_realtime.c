/* SPDX-License-Identifier: GPL-3.0-only */
/* Host scheduling experiment over the REAL byte queue; no decoder or PS4. */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/stream_au_queue.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { OFFERED_AUS=180, FPS=60, SPS_BYTES=19, PPS_BYTES=8 };
static atomic_uint checks;
#define CHECK(x) do { atomic_fetch_add_explicit(&checks,1,memory_order_relaxed); \
    if(!(x)){fprintf(stderr,"realtime queue check line %d: %s\n",__LINE__,#x);abort();} } while(0)

static uint64_t clock_ns(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static void sleep_until(uint64_t ns)
{
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
    int rc;do rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL);while(rc==EINTR);
    CHECK(!rc);
}
static void sleep_us(uint64_t us){sleep_until(clock_ns()+us*1000);}
static uint8_t sample(uint64_t id,size_t at,unsigned salt)
{return (uint8_t)((id*29+(uint64_t)at*17+salt*43)&255);}
static void fill(uint8_t *p,size_t n,uint64_t id,unsigned salt)
{for(size_t i=0;i<n;++i)p[i]=sample(id,i,salt);}
static bool matches(const uint8_t *p,size_t n,uint64_t id,unsigned salt)
{for(size_t i=0;i<n;++i)if(p[i]!=sample(id,i,salt))return false;return true;}
static const size_t sizes[]={12288,22528,34816,48128,72704,100352,184320,360448,524288};
static size_t payload_size(uint64_t id){return sizes[(id-1)%(sizeof(sizes)/sizeof(*sizes))];}
static bool periodic_idr(uint64_t id){return (id-1)%60==0;}

static X4StreamAuInput prepare(uint8_t *data,uint8_t *sps,uint8_t *pps,
    uint64_t id,size_t bytes,bool idr,uint64_t now_us)
{
    fill(data,bytes,id,1);fill(sps,SPS_BYTES,id,2);fill(pps,PPS_BYTES,id,3);
    return (X4StreamAuInput){.data=data,.sps=sps,.pps=pps,.bytes=bytes,
        .sps_bytes=SPS_BYTES,.pps_bytes=PPS_BYTES,.pts_us=(id-1)*1000000/FPS,
        .identity=id,.arrival_us=now_us,.rtp_timestamp=(uint32_t)((id-1)*1500),
        .rtp_timestamp_valid=true,.is_idr=idr};
}
static void poison(uint8_t *data,size_t n,uint8_t *sps,uint8_t *pps)
{memset(data,0xa5,n);memset(sps,0x5a,SPS_BYTES);memset(pps,0xc3,PPS_BYTES);}
static void verify(const X4StreamAuView *v,const uint8_t *source,const uint8_t *sps,
    const uint8_t *pps,size_t expected_bytes,bool expected_idr)
{
    CHECK(v->data!=source&&v->sps!=sps&&v->pps!=pps);
    CHECK(v->bytes==expected_bytes&&v->sps_bytes==SPS_BYTES&&v->pps_bytes==PPS_BYTES);
    CHECK(v->is_idr==expected_idr&&v->pts_us==(v->identity-1)*1000000/FPS);
    CHECK(v->rtp_timestamp_valid&&v->rtp_timestamp==(uint32_t)((v->identity-1)*1500));
    CHECK(matches(v->data,v->bytes,v->identity,1));
    CHECK(matches(v->sps,v->sps_bytes,v->identity,2)&&matches(v->pps,v->pps_bytes,v->identity,3));
}

/* A separate deterministic prelude fills exactly the entire 8 MiB arena with
 * variable real payloads and parameters. It does not claim realtime timing.
 * Every caller buffer is overwritten before any consumer reads its view. */
static void capacity_and_copy_prelude(void)
{
    static const size_t extents[]={2097152,1572864,1048576,524288,2097152,1048576};
    X4StreamAuQueue *q=x4_stream_au_queue_create();CHECK(q);
    uint8_t *source=malloc(X4_STREAM_AU_MAX_BYTES);CHECK(source);
    uint8_t sps[SPS_BYTES],pps[PPS_BYTES];
    size_t total=0;
    for(unsigned i=0;i<sizeof(extents)/sizeof(*extents);++i){
        X4StreamAuInput input=prepare(source,sps,pps,i+1,extents[i],i==0,0);
        input.rtp_timestamp_valid=false; /* Capacity test, no advancing content clock. */
        X4StreamAuOutcome out={0};CHECK(x4_stream_au_queue_try_push(q,&input,0,&out)==X4_STREAM_AU_OK);
        poison(source,input.bytes,sps,pps);total+=input.bytes;
    }
    X4StreamAuStats stats={0};CHECK(!x4_stream_au_queue_try_snapshot(q,&stats));
    CHECK(total==8388608&&stats.arena_bytes==total&&stats.resident_bytes==total&&stats.payload_bytes==total);
    CHECK(stats.admitted==6&&stats.ready_entries==6&&!stats.fatal&&!stats.waiting_idr);
    for(unsigned i=0;i<sizeof(extents)/sizeof(*extents);++i){
        X4StreamAuLease lease;X4StreamAuView view;X4StreamAuOutcome out={0};
        CHECK(!x4_stream_au_queue_try_take(q,0,&lease,&view,&out));
        CHECK(view.identity==i+1&&view.bytes==extents[i]&&view.data!=source&&view.sps!=sps&&view.pps!=pps);
        CHECK(matches(view.data,view.bytes,view.identity,1));
        CHECK(matches(view.sps,view.sps_bytes,view.identity,2)&&matches(view.pps,view.pps_bytes,view.identity,3));
        CHECK(!x4_stream_au_queue_release(q,lease));
    }
    x4_stream_au_queue_stop(q);CHECK(x4_stream_au_queue_quiescent(q));
    CHECK(x4_stream_au_queue_destroy(q));free(source);
}

typedef struct {
    X4StreamAuQueue *queue;
    pthread_barrier_t begin;
    atomic_bool producer_done;
    uint8_t *source,sps[SPS_BYTES],pps[PPS_BYTES];
    uint64_t offered,accepted,offered_bytes,accepted_bytes,idrs_admitted;
    uint64_t admission_ns,admission_max_ns,schedule_late_max_ns,producer_span_ns;
    uint64_t consumer_calls,consumer_long_calls,consumer_empty,consumer_busy;
    uint64_t consumer_sleep_ns,consumer_sleep_max_ns;
} Case;
static void begin_together(Case *c)
{int rc=pthread_barrier_wait(&c->begin);CHECK(rc==0||rc==PTHREAD_BARRIER_SERIAL_THREAD);}

static void *producer(void *arg)
{
    Case *c=arg;begin_together(c);uint64_t start=clock_ns();
    for(uint64_t i=0;i<OFFERED_AUS;++i){
        uint64_t target=start+i*UINT64_C(1000000000)/FPS;
        sleep_until(target);uint64_t actual=clock_ns();
        if(actual-target>c->schedule_late_max_ns)c->schedule_late_max_ns=actual-target;
        uint64_t id=i+1;size_t bytes=payload_size(id);
        X4StreamAuInput input=prepare(c->source,c->sps,c->pps,id,bytes,periodic_idr(id),actual/1000);
        X4StreamAuOutcome out={0};uint64_t before=clock_ns();
        /* Exactly one attempt per scheduled AU: no retry/backoff/queue wait. */
        int rc=x4_stream_au_queue_try_push(c->queue,&input,before/1000,&out);
        uint64_t elapsed=clock_ns()-before;c->admission_ns+=elapsed;
        if(elapsed>c->admission_max_ns)c->admission_max_ns=elapsed;
        CHECK(rc>=0&&rc!=X4_STREAM_AU_STOPPED&&rc!=X4_STREAM_AU_EMPTY);
        ++c->offered;c->offered_bytes+=bytes;
        if(rc==X4_STREAM_AU_OK){++c->accepted;c->accepted_bytes+=bytes;c->idrs_admitted+=input.is_idr;}
        poison(c->source,bytes,c->sps,c->pps);
    }
    sleep_until(start+UINT64_C(3000000000));c->producer_span_ns=clock_ns()-start;
    atomic_store_explicit(&c->producer_done,true,memory_order_release);return NULL;
}
static void *consumer(void *arg)
{
    Case *c=arg;begin_together(c);
    for(;;){
        X4StreamAuLease lease;X4StreamAuView view;X4StreamAuOutcome out={0};
        int rc=x4_stream_au_queue_try_take(c->queue,clock_ns()/1000,&lease,&view,&out);
        CHECK(rc>=0&&rc!=X4_STREAM_AU_STOPPED);
        if(rc==X4_STREAM_AU_OK){
            ++c->consumer_calls;CHECK(view.identity>=1&&view.identity<=OFFERED_AUS);
            verify(&view,c->source,c->sps,c->pps,payload_size(view.identity),periodic_idr(view.identity));
            uint64_t work=c->consumer_calls%23==0?90000:20000;
            c->consumer_long_calls+=work==90000;
            uint64_t before=clock_ns();sleep_us(work);uint64_t elapsed=clock_ns()-before;
            c->consumer_sleep_ns+=elapsed;if(elapsed>c->consumer_sleep_max_ns)c->consumer_sleep_max_ns=elapsed;
            /* Lease remains pinned through simulated Decode and producer reuse. */
            verify(&view,c->source,c->sps,c->pps,payload_size(view.identity),periodic_idr(view.identity));
            CHECK(!x4_stream_au_queue_release(c->queue,lease));
        }else{
            if(rc==X4_STREAM_AU_EMPTY){++c->consumer_empty;
                if(atomic_load_explicit(&c->producer_done,memory_order_acquire))break;
            }else if(rc==X4_STREAM_AU_BUSY)++c->consumer_busy;
            sleep_us(100); /* Same empty-poll cadence for both implementations. */
        }
    }
    return NULL;
}
int main(int argc,char **argv)
{
    const char *label=argc>1?argv[1]:"current";CHECK(strlen(label)<64);
    for(const char *p=label;*p;++p)CHECK((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-');
    capacity_and_copy_prelude();Case c={0};c.queue=x4_stream_au_queue_create();CHECK(c.queue);
    c.source=malloc(X4_STREAM_AU_MAX_BYTES);CHECK(c.source);
    atomic_init(&c.producer_done,false);CHECK(!pthread_barrier_init(&c.begin,NULL,3));
    pthread_t writer,reader;uint64_t wall_begin=clock_ns();
    CHECK(!pthread_create(&reader,NULL,consumer,&c));CHECK(!pthread_create(&writer,NULL,producer,&c));
    begin_together(&c);CHECK(!pthread_join(writer,NULL));CHECK(!pthread_join(reader,NULL));
    uint64_t wall_ns=clock_ns()-wall_begin;
    x4_stream_au_queue_stop(c.queue);X4StreamAuStats s={0};CHECK(!x4_stream_au_queue_try_snapshot(c.queue,&s));
    CHECK(c.offered==OFFERED_AUS&&c.producer_span_ns>=UINT64_C(3000000000)&&wall_ns>=c.producer_span_ns);
    CHECK(!s.fatal&&s.stopped&&!s.inflight_entries&&s.arena_bytes==8388608);
    CHECK(s.admitted==c.accepted&&s.taken==c.consumer_calls&&s.released==s.taken);
    uint64_t incoming=s.rejected_incoming_full+s.rejected_incoming_late+s.rejected_incoming_wait_idr+s.rejected_incoming_busy;
    uint64_t retired=s.retired_ready_full+s.retired_ready_late+s.retired_ready_busy;
    CHECK(s.admitted+incoming==c.offered&&retired==s.evicted_non_idr+s.old_idr_retired);
    CHECK(s.admitted==s.taken+retired+s.ready_entries);
    CHECK(s.recovery_transitions>=(unsigned)s.waiting_idr);
    uint64_t recovered=s.recovery_transitions-(unsigned)s.waiting_idr;
    CHECK(recovered<=c.idrs_admitted);
    printf("{\"schema\":\"X4_STREAM_AU_REALTIME_HOST1\",\"implementation\":\"%s\","
        "\"scope\":\"real pthread scheduling and real copies over actual AU queue; synthetic payload, periodic IDR each second; no decoder, network or PS4; scheduling can change all timings and recovery counts; comparison is descriptive\","
        "\"capacity_prelude_bytes_verified\":8388608,\"capacity_prelude_variable_AUs\":6,"
        "\"offered\":%llu,\"accepted\":%llu,\"taken\":%llu,\"released\":%llu,\"remaining_ready\":%u,"
        "\"rejected_incoming_busy\":%llu,\"rejected_incoming_full\":%llu,\"rejected_incoming_late\":%llu,\"rejected_incoming_wait_idr\":%llu,"
        "\"recovery_episodes\":%llu,\"recovered_episodes\":%llu,\"open_recovery_episode\":%s,\"queue_keyframe_intentions\":%llu,"
        "\"retired_ready\":%llu,\"retired_ready_busy\":%llu,\"retired_ready_full\":%llu,\"retired_ready_late\":%llu,"
        "\"offered_bytes_copied_by_caller\":%llu,\"accepted_payload_bytes_copied_by_queue\":%llu,"
        "\"producer_calls\":%llu,\"producer_active_us\":%.3f,\"case_wall_us\":%.3f,"
        "\"admission_mean_us\":%.3f,\"admission_max_observed_us\":%.3f,\"schedule_late_max_us\":%.3f,"
        "\"decode_model_normal_us\":20000,\"decode_model_long_us\":90000,\"decode_model_long_every\":23,"
        "\"decode_model_calls\":%llu,\"decode_model_long_calls\":%llu,\"decode_sleep_mean_observed_us\":%.3f,\"decode_sleep_max_observed_us\":%.3f,"
        "\"consumer_empty_polls\":%llu,\"consumer_busy_polls\":%llu,\"queue_depth_max\":%u,\"queue_lag_max_us\":%llu,\"checks\":%u}\n",
        label,(unsigned long long)c.offered,(unsigned long long)s.admitted,(unsigned long long)s.taken,(unsigned long long)s.released,s.ready_entries,
        (unsigned long long)s.rejected_incoming_busy,(unsigned long long)s.rejected_incoming_full,(unsigned long long)s.rejected_incoming_late,(unsigned long long)s.rejected_incoming_wait_idr,
        (unsigned long long)s.recovery_transitions,(unsigned long long)recovered,s.waiting_idr?"true":"false",(unsigned long long)s.request_intentions,
        (unsigned long long)retired,(unsigned long long)s.retired_ready_busy,(unsigned long long)s.retired_ready_full,(unsigned long long)s.retired_ready_late,
        (unsigned long long)c.offered_bytes,(unsigned long long)c.accepted_bytes,(unsigned long long)c.offered,
        c.producer_span_ns/1000.,wall_ns/1000.,c.admission_ns/(1000.*c.offered),c.admission_max_ns/1000.,c.schedule_late_max_ns/1000.,
        (unsigned long long)c.consumer_calls,(unsigned long long)c.consumer_long_calls,c.consumer_calls?c.consumer_sleep_ns/(1000.*c.consumer_calls):0.,c.consumer_sleep_max_ns/1000.,
        (unsigned long long)c.consumer_empty,(unsigned long long)c.consumer_busy,s.depth_max,(unsigned long long)s.lag_max_us,atomic_load(&checks));
    CHECK(x4_stream_au_queue_quiescent(c.queue)&&x4_stream_au_queue_destroy(c.queue));
    CHECK(!pthread_barrier_destroy(&c.begin));free(c.source);return 0;
}
