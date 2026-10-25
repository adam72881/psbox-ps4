/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/streaming/keyframe_limiter.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"keyframe check %d: %s\n",__LINE__,#x);abort();} } while(0)

static X4KeyframeLimiterSnapshot snapshot(X4KeyframeLimiter *l)
{X4KeyframeLimiterSnapshot s;x4_keyframe_limiter_snapshot(l,&s);return s;}

/* Independent last-ten oracle: any eleven successive grants must span at
 * least five seconds, even when every request has new exception evidence. */
typedef struct {uint64_t times[10];unsigned count,at;} Oracle;
static void record(Oracle *o,uint64_t now)
{
    if(o->count==10)CHECK(now>=o->times[o->at]&&now-o->times[o->at]>=5000000);
    else ++o->count;
    o->times[o->at]=now;o->at=(o->at+1)%10;
}

static void basic_and_pending(void)
{
    X4KeyframeLimiter l;X4KeyframeGrant g;x4_keyframe_limiter_init(&l);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
    CHECK(x4_keyframe_limiter_take(&l,0,&g));
    CHECK(g.sequence==1&&g.at_us==0&&!g.exception&&g.reasons==X4_KEYFRAME_ASSEMBLY);
    CHECK(!x4_keyframe_limiter_take(&l,1,&g));
    CHECK(g.sequence==0&&snapshot(&l).deferred==0);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY|X4_KEYFRAME_STARTUP);
    CHECK(!x4_keyframe_limiter_take(&l,499999,&g));
    CHECK(snapshot(&l).pending_reasons==(X4_KEYFRAME_PIPELINE|X4_KEYFRAME_ASSEMBLY|X4_KEYFRAME_STARTUP));
    CHECK(x4_keyframe_limiter_take(&l,500000,&g));
    CHECK(!g.exception&&g.sequence==2);
    X4KeyframeLimiterSnapshot s=snapshot(&l);
    CHECK(s.requested==3&&s.coalesced==1&&s.granted==2&&s.deferred==1);
    CHECK(s.interval_deferred==1&&!s.cap_deferred&&!s.clock_deferred&&!s.pending_reasons);
    x4_keyframe_limiter_request(&l,0);x4_keyframe_limiter_request(&l,1u<<20);
    CHECK(snapshot(&l).requested==3&&!snapshot(&l).counter_overflow);
    x4_keyframe_limiter_init(NULL);x4_keyframe_limiter_request(NULL,1);
    x4_keyframe_limiter_observe_gap(NULL);x4_keyframe_limiter_observe_idr(NULL);
    CHECK(!x4_keyframe_limiter_take(NULL,0,NULL));
    x4_keyframe_limiter_snapshot(NULL,&s);CHECK(!s.requested&&!s.pending_reasons);
    x4_keyframe_limiter_snapshot(&l,NULL);
}

static void exception_generations(void)
{
    X4KeyframeLimiter l;X4KeyframeGrant g;x4_keyframe_limiter_init(&l);
    x4_keyframe_limiter_observe_gap(&l);
    CHECK(!x4_keyframe_limiter_take(&l,0,&g)); /* Evidence is not an intent. */
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_AU_QUEUE);
    CHECK(x4_keyframe_limiter_take(&l,0,&g)&&!g.exception&&g.gap_generation==1);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);
    CHECK(!x4_keyframe_limiter_take(&l,1,&g)); /* Old evidence cannot be reused. */
    x4_keyframe_limiter_observe_gap(&l);
    CHECK(x4_keyframe_limiter_take(&l,1,&g)&&g.exception&&g.gap_generation==2);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);
    CHECK(!x4_keyframe_limiter_take(&l,2,&g));
    x4_keyframe_limiter_observe_idr(&l);
    CHECK(!x4_keyframe_limiter_take(&l,2,&g)); /* Useful IDR retires the old retry. */
    CHECK(snapshot(&l).retired_on_idr==1);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE); /* A post-IDR new intent. */
    CHECK(x4_keyframe_limiter_take(&l,2,&g)&&g.exception&&g.idr_generation==1);
    x4_keyframe_limiter_observe_idr(&l);
    CHECK(!x4_keyframe_limiter_take(&l,3,&g));
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
    CHECK(x4_keyframe_limiter_take(&l,3,&g)&&g.exception&&g.idr_generation==2);
    CHECK(snapshot(&l).granted==4);
}

static void idr_retires_retry_but_not_new_loss(void)
{
    X4KeyframeLimiter l;X4KeyframeGrant g;x4_keyframe_limiter_init(&l);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_STARTUP);
    CHECK(x4_keyframe_limiter_take(&l,0,&g));
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
    x4_keyframe_limiter_observe_idr(&l);
    CHECK(!x4_keyframe_limiter_take(&l,100,&g));
    CHECK(snapshot(&l).retired_on_idr==1&&!snapshot(&l).pending_reasons);
    x4_keyframe_limiter_observe_gap(&l);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_AU_QUEUE);
    CHECK(x4_keyframe_limiter_take(&l,101,&g)&&g.exception);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);
    x4_keyframe_limiter_observe_gap(&l);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
    x4_keyframe_limiter_observe_idr(&l); /* New loss before/during IDR is preserved. */
    CHECK(snapshot(&l).pending_reasons==(X4_KEYFRAME_PIPELINE|X4_KEYFRAME_ASSEMBLY));
    CHECK(x4_keyframe_limiter_take(&l,102,&g)&&g.exception);
    X4KeyframeLimiterSnapshot s=snapshot(&l);
    CHECK(s.retired_on_idr==1&&s.granted==3&&s.requested==6&&s.coalesced==2);
}

static void normal_five_second_storm(void)
{
    X4KeyframeLimiter l;Oracle o={0};X4KeyframeGrant g;x4_keyframe_limiter_init(&l);
    for(uint64_t t=0;t<5000000;t+=1000){
        x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
        if(x4_keyframe_limiter_take(&l,t,&g)){CHECK(!g.exception);record(&o,t);}
    }
    X4KeyframeLimiterSnapshot s=snapshot(&l);
    CHECK(s.requested==5000&&s.coalesced==4989&&s.granted==10&&s.deferred==4990);
    CHECK(s.cap_deferred==499&&s.interval_deferred==4491&&s.pending_reasons==X4_KEYFRAME_ASSEMBLY);
    CHECK(x4_keyframe_limiter_take(&l,5000000,&g));record(&o,5000000);
    CHECK(!g.exception&&g.sequence==11);
}

static void exceptions_cannot_reset_hard_cap(void)
{
    X4KeyframeLimiter l;Oracle o={0};X4KeyframeGrant g;x4_keyframe_limiter_init(&l);
    for(uint64_t t=0;t<5000000;t+=1000){
        if(t&1024)x4_keyframe_limiter_observe_idr(&l);
        else x4_keyframe_limiter_observe_gap(&l);
        x4_keyframe_limiter_request(&l,X4_KEYFRAME_AU_QUEUE|X4_KEYFRAME_PIPELINE);
        if(x4_keyframe_limiter_take(&l,t,&g))record(&o,t);
    }
    X4KeyframeLimiterSnapshot s=snapshot(&l);
    CHECK(s.granted==10&&s.deferred==4990&&s.cap_deferred==4990&&!s.interval_deferred);
    CHECK(s.gap_generation+s.idr_generation==5000&&s.pending_reasons);
    CHECK(!x4_keyframe_limiter_take(&l,4999999,&g));
    for(uint64_t t=5000000;t<=5009000;t+=1000){
        x4_keyframe_limiter_observe_gap(&l);x4_keyframe_limiter_observe_idr(&l);
        x4_keyframe_limiter_request(&l,X4_KEYFRAME_STARTUP);
        CHECK(x4_keyframe_limiter_take(&l,t,&g));record(&o,t);
    }
    CHECK(snapshot(&l).granted==20);
    x4_keyframe_limiter_observe_idr(&l);x4_keyframe_limiter_request(&l,X4_KEYFRAME_NO_MEDIA);
    CHECK(!x4_keyframe_limiter_take(&l,5010000,&g));
    CHECK(snapshot(&l).pending_reasons==X4_KEYFRAME_NO_MEDIA);
}

static void exact_boundary_and_failed_transport(void)
{
    X4KeyframeLimiter l;X4KeyframeGrant g;x4_keyframe_limiter_init(&l);
    for(uint64_t i=0;i<10;++i){
        x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
        CHECK(x4_keyframe_limiter_take(&l,i*500000,&g));
        x4_keyframe_limiter_note_transport(&l,true,-1,true,-2);
    }
    x4_keyframe_limiter_observe_gap(&l);x4_keyframe_limiter_observe_idr(&l);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_AU_QUEUE);
    CHECK(!x4_keyframe_limiter_take(&l,4999999,&g));
    CHECK(snapshot(&l).granted==10&&snapshot(&l).transport_decisions==10);
    CHECK(x4_keyframe_limiter_take(&l,5000000,&g));
    x4_keyframe_limiter_note_transport(&l,true,4,true,-1);
    x4_keyframe_limiter_note_transport(&l,false,0,false,0);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);x4_keyframe_limiter_observe_gap(&l);
    CHECK(!x4_keyframe_limiter_take(&l,5000001,&g)); /* Next old timestamp still lives. */
    CHECK(x4_keyframe_limiter_take(&l,5500000,&g));
    x4_keyframe_limiter_note_transport(&l,false,-1,true,0);
    X4KeyframeLimiterSnapshot s=snapshot(&l);
    CHECK(s.granted==12&&s.transport_decisions==12);
    CHECK(s.control_attempts==11&&s.control_errors==10&&s.control_local_successes==1);
    CHECK(s.pli_attempts==12&&s.pli_errors==11&&s.pli_local_successes==1);
    CHECK(s.control_attempts==s.control_errors+s.control_local_successes);
    CHECK(s.pli_attempts==s.pli_errors+s.pli_local_successes);
}

static void clock_and_overflow(void)
{
    X4KeyframeLimiter l;X4KeyframeGrant g;x4_keyframe_limiter_init(&l);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
    CHECK(x4_keyframe_limiter_take(&l,UINT64_MAX-500000,&g));
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);x4_keyframe_limiter_observe_gap(&l);
    CHECK(!x4_keyframe_limiter_take(&l,0,&g));
    CHECK(snapshot(&l).clock_deferred==1&&snapshot(&l).pending_reasons);
    CHECK(x4_keyframe_limiter_take(&l,UINT64_MAX,&g)&&!g.exception);
    x4_keyframe_limiter_init(&l);
    atomic_store(&l.gap_generation,UINT_FAST64_MAX);
    x4_keyframe_limiter_observe_gap(&l);
    CHECK(snapshot(&l).counter_overflow);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
    CHECK(x4_keyframe_limiter_take(&l,0,&g));
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);x4_keyframe_limiter_observe_idr(&l);
    CHECK(!x4_keyframe_limiter_take(&l,1,&g)); /* Overflow disables early exceptions. */
    CHECK(x4_keyframe_limiter_take(&l,500000,&g));
}

typedef struct {unsigned count;uint64_t times[3];} Observation;
static void observe(void *arg,uint64_t now)
{
    Observation *o=arg;CHECK(o->count<3);o->times[o->count++]=now;
}
static void reservation_observer(void)
{
    X4KeyframeLimiter l;Observation o={0};x4_keyframe_limiter_init(&l);
    x4_keyframe_limiter_set_observer(NULL,observe,&o);
    x4_keyframe_limiter_set_observer(&l,observe,&o);
    x4_keyframe_limiter_observe_gap(&l);x4_keyframe_limiter_observe_idr(&l);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);CHECK(o.count==0);
    CHECK(x4_keyframe_limiter_take(&l,0,NULL));
    CHECK(o.count==1&&o.times[0]==0&&o.count==snapshot(&l).granted);
    x4_keyframe_limiter_note_transport(&l,true,-1,true,-1);CHECK(o.count==1);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_PIPELINE);
    CHECK(!x4_keyframe_limiter_take(&l,499999,NULL)&&o.count==1);
    CHECK(x4_keyframe_limiter_take(&l,500000,NULL));
    CHECK(o.count==2&&o.times[1]==500000&&o.count==snapshot(&l).granted);
    CHECK(!x4_keyframe_limiter_take(&l,1000000,NULL)&&o.count==2);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_STARTUP);x4_keyframe_limiter_observe_gap(&l);
    CHECK(x4_keyframe_limiter_take(&l,500001,NULL));
    CHECK(o.count==3&&o.times[2]==500001&&o.count==snapshot(&l).granted);
    x4_keyframe_limiter_init(&l);x4_keyframe_limiter_set_observer(&l,NULL,&o);
    x4_keyframe_limiter_request(&l,X4_KEYFRAME_ASSEMBLY);
    CHECK(x4_keyframe_limiter_take(&l,0,NULL)&&o.count==3);
}

enum { PRODUCERS=4, INTENTS=10000 };
typedef struct {X4KeyframeLimiter *l;atomic_bool *go;atomic_uint *done;unsigned reason;} Producer;
static void *produce(void *arg)
{
    Producer *p=arg;
    while(!atomic_load_explicit(p->go,memory_order_acquire))sched_yield();
    for(unsigned i=0;i<INTENTS;++i)x4_keyframe_limiter_request(p->l,p->reason);
    atomic_fetch_add_explicit(p->done,1,memory_order_release);return NULL;
}
static void concurrent_publication(void)
{
    X4KeyframeLimiter l;Oracle o={0};x4_keyframe_limiter_init(&l);
    atomic_bool go;atomic_uint done;atomic_init(&go,false);atomic_init(&done,0);
    pthread_t threads[PRODUCERS];Producer args[PRODUCERS];
    for(unsigned i=0;i<PRODUCERS;++i){
        args[i]=(Producer){&l,&go,&done,1u<<i};
        CHECK(!pthread_create(&threads[i],NULL,produce,&args[i]));
    }
    atomic_store_explicit(&go,true,memory_order_release);
    uint64_t t=0;
    while(atomic_load_explicit(&done,memory_order_acquire)!=PRODUCERS||snapshot(&l).pending_reasons){
        if(x4_keyframe_limiter_take(&l,t,NULL))record(&o,t);
        t+=1000;sched_yield();
    }
    for(unsigned i=0;i<PRODUCERS;++i)CHECK(!pthread_join(threads[i],NULL));
    X4KeyframeLimiterSnapshot s=snapshot(&l);
    CHECK(s.requested==PRODUCERS*INTENTS&&s.requested==s.coalesced+s.granted);
    CHECK(!s.pending_reasons&&!s.counter_overflow&&s.granted>0);
    CHECK(s.deferred==s.interval_deferred+s.cap_deferred+s.clock_deferred);
}

static void *post_fresh_gap(void *arg)
{
    Producer *p=arg;
    while(!atomic_load_explicit(p->go,memory_order_acquire))sched_yield();
    x4_keyframe_limiter_observe_gap(p->l);
    x4_keyframe_limiter_request(p->l,X4_KEYFRAME_AU_QUEUE);
    return NULL;
}
static void concurrent_gap_and_idr(void)
{
    for(unsigned n=0;n<128;++n){
        X4KeyframeLimiter l;x4_keyframe_limiter_init(&l);
        x4_keyframe_limiter_request(&l,X4_KEYFRAME_STARTUP);
        CHECK(x4_keyframe_limiter_take(&l,0,NULL));
        atomic_bool go;atomic_init(&go,false);Producer p={.l=&l,.go=&go};pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,post_fresh_gap,&p));
        atomic_store_explicit(&go,true,memory_order_release);
        x4_keyframe_limiter_observe_idr(&l);
        CHECK(!pthread_join(thread,NULL));
        CHECK(snapshot(&l).pending_reasons==X4_KEYFRAME_AU_QUEUE);
        X4KeyframeGrant g;CHECK(x4_keyframe_limiter_take(&l,1,&g)&&g.exception);
        CHECK(snapshot(&l).granted==2&&!snapshot(&l).pending_reasons);
    }
}

int main(void)
{
    basic_and_pending();exception_generations();idr_retires_retry_but_not_new_loss();normal_five_second_storm();
    exceptions_cannot_reset_hard_cap();exact_boundary_and_failed_transport();
    clock_and_overflow();reservation_observer();concurrent_publication();concurrent_gap_and_idr();
    printf("PASS keyframe limiter: %u checks; 500ms normal, hard10/rolling5s, pending/coalescing, exceptions, failed local sends, MPSC publication.\n",checks);
    return 0;
}
