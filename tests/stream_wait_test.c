/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/stream_wait.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t wall_us(void)
{struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000u;}
static void pause_us(unsigned us)
{struct timespec t={us/1000000u,(long)(us%1000000u)*1000L};assert(!nanosleep(&t,NULL));}
typedef struct {
    X4StreamWait wait;
    atomic_uint published,consumed;
    atomic_bool finished;
    unsigned target;
    uint64_t exit_us;
} Fixture;
static void *consumer(void *arg)
{Fixture *f=arg;
 for(;;){
    unsigned available=atomic_load_explicit(&f->published,memory_order_acquire);
    unsigned used=atomic_load_explicit(&f->consumed,memory_order_relaxed);
    if(used<available){atomic_store_explicit(&f->consumed,used+1,memory_order_release);continue;}
    if(atomic_load_explicit(&f->finished,memory_order_acquire))break;
    int rc=x4_stream_wait_block(&f->wait,20000);
    assert(rc==X4_STREAM_WAIT_EVENT||rc==X4_STREAM_WAIT_TIMEOUT||rc==X4_STREAM_WAIT_CLOSED);
 }
 f->exit_us=wall_us();return NULL;}
static void create(Fixture *f)
{memset(f,0,sizeof(*f));atomic_init(&f->published,0);atomic_init(&f->consumed,0);atomic_init(&f->finished,false);
 assert(!x4_stream_wait_init(&f->wait,"x4-wait-host"));}
static void retained_and_coalesced(void)
{Fixture f;create(&f);assert(!x4_stream_wait_notify(&f.wait));
 for(unsigned i=0;i<10000;++i)assert(!x4_stream_wait_notify(&f.wait));
 assert(x4_stream_wait_block(&f.wait,20000)==X4_STREAM_WAIT_EVENT);
 assert(x4_stream_wait_block(&f.wait,1000)==X4_STREAM_WAIT_TIMEOUT);
 X4StreamWaitStats s;x4_stream_wait_snapshot(&f.wait,&s);
 assert(s.notifications==10001&&s.coalesced==10000&&s.wakeups==2&&s.timeouts==1&&!s.errors);
 assert(!x4_stream_wait_destroy(&f.wait));puts("PASS retained token before wait + 10000 coalesced notifications");}
static void stressed_producer_consumer(void)
{for(unsigned round=0;round<8;++round){Fixture f;create(&f);pthread_t thread;assert(!pthread_create(&thread,NULL,consumer,&f));
 const unsigned count=100000;
 for(unsigned i=1;i<=count;++i){atomic_store_explicit(&f.published,i,memory_order_release);
    assert(!x4_stream_wait_notify(&f.wait));if(!(i%337))pause_us(1);}
 uint64_t deadline=wall_us()+3000000;
 while(atomic_load_explicit(&f.consumed,memory_order_acquire)!=count){assert(wall_us()<deadline);pause_us(100);}
 atomic_store_explicit(&f.finished,true,memory_order_release);assert(!x4_stream_wait_notify(&f.wait));
 assert(!pthread_join(thread,NULL));assert(!x4_stream_wait_destroy(&f.wait));
 }
 puts("PASS stressed producer/consumer 800000 publications; every predicate consumed, no lost work/deadlock");}
typedef struct {
    unsigned next;
    uint64_t deadline_us;
} TimeoutObservation;
typedef struct {
    Fixture fixture;
    uint64_t *published_us,*notified_us;
    TimeoutObservation *timeouts;
    unsigned timeout_count,timeout_capacity;
} AtomicStress;
static void *atomic_predicate_consumer(void *argument)
{
 AtomicStress *s=argument;Fixture *f=&s->fixture;
 for(;;){
    unsigned available=atomic_load_explicit(&f->published,memory_order_acquire);
    unsigned used=atomic_load_explicit(&f->consumed,memory_order_relaxed);
    if(used<available){atomic_store_explicit(&f->consumed,used+1,memory_order_release);continue;}
    if(atomic_load_explicit(&f->finished,memory_order_acquire))break;
    /* This deadline is earlier than the semaphore's internal deadline. Work
     * arriving after it cannot be blamed on a lost wake, even if the consumer
     * is descheduled before observing the timeout return. */
    uint64_t deadline=wall_us()+X4_STREAM_WAIT_MAX_US;
    int rc=x4_stream_wait_block(&f->wait,X4_STREAM_WAIT_MAX_US);
    assert(rc==X4_STREAM_WAIT_EVENT||rc==X4_STREAM_WAIT_TIMEOUT);
    if(rc==X4_STREAM_WAIT_TIMEOUT){
        available=atomic_load_explicit(&f->published,memory_order_acquire);
        if(available>used){
            assert(s->timeout_count<s->timeout_capacity);
            s->timeouts[s->timeout_count++]=(TimeoutObservation){used+1,deadline};
        }
    }
 }
 return NULL;
}
static void millions_of_atomic_predicates(void)
{
 const unsigned rounds=4,count=500000,batches[]={1,7,31,127};
 uint64_t observed_timeouts=0,late_publications=0,late_notifications=0,missed=0;
 for(unsigned round=0;round<rounds;++round){
    AtomicStress s={0};create(&s.fixture);s.timeout_capacity=4096;
    s.published_us=calloc((size_t)count+1,sizeof(*s.published_us));
    s.notified_us=calloc((size_t)count+1,sizeof(*s.notified_us));
    s.timeouts=calloc(s.timeout_capacity,sizeof(*s.timeouts));
    assert(s.published_us&&s.notified_us&&s.timeouts);
    pthread_t thread;assert(!pthread_create(&thread,NULL,atomic_predicate_consumer,&s));
    uint64_t overall_deadline=wall_us()+30000000;
    for(unsigned i=1;i<=count;++i){
        s.published_us[i]=wall_us();
        atomic_store_explicit(&s.fixture.published,i,memory_order_release);
        assert(!x4_stream_wait_notify(&s.fixture.wait));
        s.notified_us[i]=wall_us();
        /* Vary the atomic-only handoff to cover both empty predicates and
         * coalescing bursts. There is no mutex around publication/inspection. */
        if(!(i%batches[round]))while(atomic_load_explicit(&s.fixture.consumed,memory_order_acquire)<i){
            assert(wall_us()<overall_deadline);sched_yield();}
    }
    while(atomic_load_explicit(&s.fixture.consumed,memory_order_acquire)<count){
        assert(wall_us()<overall_deadline);sched_yield();}
    atomic_store_explicit(&s.fixture.finished,true,memory_order_release);
    assert(!x4_stream_wait_notify(&s.fixture.wait));assert(!pthread_join(thread,NULL));
    /* Inspect timestamps only after join, so the producer being descheduled
     * between publication and Signal completion cannot create a false alarm. */
    for(unsigned i=0;i<s.timeout_count;++i){TimeoutObservation t=s.timeouts[i];
        ++observed_timeouts;
        if(s.published_us[t.next]>=t.deadline_us)++late_publications;
        else if(s.notified_us[t.next]>=t.deadline_us)++late_notifications;
        else ++missed;
    }
    assert(!x4_stream_wait_destroy(&s.fixture.wait));
    free(s.published_us);free(s.notified_us);free(s.timeouts);
 }
 uint64_t published_before_deadline=observed_timeouts-late_publications;
 printf("atomic-only predicates %u publications; timeouts_with_published_work_before_deadline=%llu timeouts_with_timely_published_and_notified_work=%llu; timeout_observations=%llu late_publications=%llu late_notifications=%llu; no predicate mutex\n",
    rounds*count,(unsigned long long)(observed_timeouts-late_publications),
    (unsigned long long)missed,(unsigned long long)observed_timeouts,
    (unsigned long long)late_publications,(unsigned long long)late_notifications);
 fflush(stdout);
 /* A producer preempted before notify can explain the raw count, but that
  * run is still failed/inconclusive; never approve it as the required zero. */
 assert(!missed&&!published_before_deadline);
 puts("PASS 2000000 atomic-only publications: zero timeouts with work published before deadline");
}
static void cancellation(void)
{uint64_t maximum=0;
 for(unsigned i=0;i<64;++i){Fixture f;create(&f);pthread_t thread;assert(!pthread_create(&thread,NULL,consumer,&f));pause_us(500);
 uint64_t began=wall_us();atomic_store_explicit(&f.finished,true,memory_order_release);
 assert(!x4_stream_wait_close(&f.wait));assert(!pthread_join(thread,NULL));
 uint64_t elapsed=f.exit_us-began;if(elapsed>maximum)maximum=elapsed;assert(elapsed<=50000);
 assert(x4_stream_wait_block(&f.wait,20000)==X4_STREAM_WAIT_CLOSED);
 assert(!x4_stream_wait_destroy(&f.wait));}
 printf("PASS cancellation 64 trials max_us=%llu <=50000 (wait-only; not native Decode)\n",(unsigned long long)maximum);}
static void idle_wake_rate(void)
{Fixture f;create(&f);uint64_t began=wall_us();
 while(wall_us()-began<300000)assert(x4_stream_wait_block(&f.wait,20000)==X4_STREAM_WAIT_TIMEOUT);
 uint64_t elapsed=wall_us()-began;X4StreamWaitStats s;x4_stream_wait_snapshot(&f.wait,&s);
 double rate=(double)s.wakeups*1000000.0/(double)elapsed;
 assert(s.wakeups>=10&&s.wakeups<=20&&rate<60.0);
 printf("PASS idle wait wakeups=%llu elapsed_us=%llu wakeups_per_s=%.3f (20ms timeout; not 100us polling)\n",
    (unsigned long long)s.wakeups,(unsigned long long)elapsed,rate);
 assert(!x4_stream_wait_destroy(&f.wait));}
static void init_failure_and_mutex(void)
{X4StreamWait w;x4_stream_wait_test_fail_init(ENOMEM);assert(x4_stream_wait_init(&w,"fail")==-ENOMEM);
 assert(!w.initialized&&x4_stream_wait_block(&w,20000)==-EINVAL&&x4_stream_wait_notify(&w)==-EINVAL);
 assert(!x4_stream_wait_destroy(&w));X4StreamMutex mutex;assert(!x4_stream_mutex_init(&mutex,"host"));
 assert(!x4_stream_mutex_lock(&mutex));assert(x4_stream_mutex_trylock(&mutex)==EBUSY);
 assert(!x4_stream_mutex_unlock(&mutex));assert(!x4_stream_mutex_trylock(&mutex));
 assert(!x4_stream_mutex_unlock(&mutex));assert(!x4_stream_mutex_destroy(&mutex));
 puts("PASS failed creation no polling fallback + mutex try-only busy path");}
int main(void)
{retained_and_coalesced();stressed_producer_consumer();millions_of_atomic_predicates();cancellation();idle_wake_rate();init_failure_and_mutex();return 0;}
