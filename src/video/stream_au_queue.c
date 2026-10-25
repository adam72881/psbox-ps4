/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_au_queue.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { AU_FREE, AU_WRITING, AU_READY, AU_INFLIGHT, AU_DONE,
    TAG_BITS = 3, RECOVERY_BITS = 3, HIST_BUCKETS = 1001, HIST_QUANTUM_US = 1000 };
typedef struct {
    atomic_uint_fast64_t tag;
    uint64_t epoch, allocation_end, pts_us, identity, arrival_us, enqueued_us;
    size_t offset, bytes, sps_bytes, pps_bytes;
    uint32_t timestamp;
    bool timestamp_valid, is_idr;
    uint8_t sps[X4_STREAM_AU_PARAMETER_BYTES], pps[X4_STREAM_AU_PARAMETER_BYTES];
} AuEntry;
typedef struct {
    atomic_uint_fast64_t admitted, taken, released;
    atomic_uint_fast64_t dropped_full, dropped_late, dropped_wait_idr, dropped_busy;
    atomic_uint_fast64_t rejected_incoming_full,rejected_incoming_late,rejected_incoming_wait_idr,rejected_incoming_busy;
    atomic_uint_fast64_t retired_ready_full,retired_ready_late,retired_ready_busy;
    atomic_uint_fast64_t evicted_non_idr, old_idr_retired, recovery_transitions;
    atomic_uint_fast64_t request_intentions, request_coalesced;
} AuCounters;
struct X4StreamAuQueue {
    uint8_t *arena;
    AuEntry entries[X4_STREAM_AU_ENTRIES];
    atomic_flag gate;
    atomic_bool accepting, fatal;
    atomic_uint active_ops;
    /* Low bit is waiting, bits1..2 hold the recovery-episode cause; upper bits
     * change on every deliberate new omission.
     * An IDR publication may clear waiting only for its exact reservation epoch. */
    atomic_uint_fast64_t recovery;
    uint64_t applied_epoch, next_generation, head, tail, arena_head, arena_tail;
    uint32_t newest_timestamp;
    bool newest_timestamp_valid;
    AuCounters counters;
    uint64_t padding_total, samples, lag_hist[HIST_BUCKETS], lag_overflow, lag_max;
    uint64_t depth_hist[X4_STREAM_AU_ENTRIES + 1];
    unsigned depth_max;
};
#ifdef X4_STREAM_AU_QUEUE_TEST_HOOKS
#define HOOK(q, e) x4_stream_au_queue_test_hook((q), (e))
#else
#define HOOK(q, e) ((void)0)
#endif
#define INC(q, field) atomic_fetch_add_explicit(&(q)->counters.field, 1, memory_order_relaxed)
#define VALUE(q, field) atomic_load_explicit(&(q)->counters.field, memory_order_relaxed)
static unsigned state(uint64_t tag) { return (unsigned)(tag & 7); }
static uint64_t tag_for(uint64_t generation, unsigned s) { return (generation << TAG_BITS) | s; }
static bool enter(X4StreamAuQueue *q)
{ return !atomic_flag_test_and_set_explicit(&q->gate, memory_order_acquire); }
static void leave(X4StreamAuQueue *q)
{ atomic_flag_clear_explicit(&q->gate, memory_order_release); }
static bool running(X4StreamAuQueue *q)
{ return atomic_load_explicit(&q->accepting, memory_order_acquire) &&
    !atomic_load_explicit(&q->fatal, memory_order_acquire); }
static X4StreamAuResult fatal(X4StreamAuQueue *q)
{
    if (q) { atomic_store_explicit(&q->fatal, true, memory_order_release);
        atomic_store_explicit(&q->accepting, false, memory_order_release); }
    return X4_STREAM_AU_FATAL;
}
static void outcome(X4StreamAuQueue *q, X4StreamAuOutcome *out, bool request)
{
    if (out) { uint64_t token=atomic_load_explicit(&q->recovery, memory_order_acquire);
        *out=(X4StreamAuOutcome){request, (token & 1)!=0, token >> RECOVERY_BITS}; }
}
/* Bounded CAS: no waiting for another owner and no sleep. An exhaustion is a
 * diagnostic fatal stop, never permission to publish with uncertain ownership. */
static bool recover(X4StreamAuQueue *q,X4StreamAuLoss cause,bool *request)
{
    uint64_t old=atomic_load_explicit(&q->recovery, memory_order_acquire);
    for (unsigned attempt=0;attempt<32;++attempt) {
        if ((old >> RECOVERY_BITS)==(UINT64_MAX >> RECOVERY_BITS)) { fatal(q); return false; }
        unsigned episode_cause=(old & 1)?(unsigned)((old>>1)&3):(unsigned)cause;
        uint64_t next=(((old >> RECOVERY_BITS)+1)<<RECOVERY_BITS)|((uint64_t)episode_cause<<1)|1;
        if (atomic_compare_exchange_strong_explicit(&q->recovery,&old,next,
                memory_order_acq_rel,memory_order_acquire)) {
            if (!(old & 1)) { INC(q,recovery_transitions);INC(q,request_intentions);*request=true; }
            else INC(q,request_coalesced);
            return true;
        }
    }
    fatal(q);return false;
}
static void retired(X4StreamAuQueue *q,bool idr,uint64_t recovery_token)
{
    if(idr)INC(q,old_idr_retired);else INC(q,evicted_non_idr);
    switch((recovery_token>>1)&3){
    case X4_STREAM_AU_LOSS_FULL:INC(q,retired_ready_full);break;
    case X4_STREAM_AU_LOSS_LATE:INC(q,retired_ready_late);break;
    case X4_STREAM_AU_LOSS_BUSY:INC(q,retired_ready_busy);break;
    default:(void)fatal(q);break;
    }
}
static void reclaim(X4StreamAuQueue *q)
{
    for (unsigned n=0;n<X4_STREAM_AU_ENTRIES && q->head<q->tail;++n) {
        AuEntry *e=&q->entries[q->head % X4_STREAM_AU_ENTRIES];
        uint64_t tag=atomic_load_explicit(&e->tag,memory_order_acquire);
        if (state(tag)!=AU_DONE) break;
        q->arena_head=e->allocation_end;
        atomic_store_explicit(&e->tag,tag_for(tag >> TAG_BITS,AU_FREE),memory_order_release);
        ++q->head;
    }
    if (q->head==q->tail) q->arena_head=q->arena_tail=0;
}
static void apply_recovery(X4StreamAuQueue *q)
{
    uint64_t recovery_token=atomic_load_explicit(&q->recovery,memory_order_acquire);
    uint64_t epoch=recovery_token>>RECOVERY_BITS;
    /* A producer publishes outside the gate. Even when this epoch was already
     * applied while that entry was WRITING, sweep its later READY publication.
     * Never turn a stale reservation into an immortal arena tombstone. */
    {
        for (uint64_t i=q->head;i<q->tail;++i) {
            AuEntry *e=&q->entries[i % X4_STREAM_AU_ENTRIES];
            uint64_t tag=atomic_load_explicit(&e->tag,memory_order_acquire);
            if (state(tag)==AU_READY && e->epoch<epoch &&
                atomic_compare_exchange_strong_explicit(&e->tag,&tag,
                    tag_for(tag >> TAG_BITS,AU_DONE),memory_order_acq_rel,memory_order_acquire)) {
                retired(q,e->is_idr,recovery_token);
            }
        }
        q->applied_epoch=epoch;
    }
    reclaim(q);
}
static unsigned live_depth(X4StreamAuQueue *q)
{
    unsigned count=0;
    for (uint64_t i=q->head;i<q->tail;++i) {
        unsigned s=state(atomic_load_explicit(&q->entries[i % X4_STREAM_AU_ENTRIES].tag,memory_order_acquire));
        if (s==AU_WRITING || s==AU_READY || s==AU_INFLIGHT) ++count;
    }
    return count;
}
static uint64_t lag(X4StreamAuQueue *q, uint64_t now)
{
    uint64_t worst=0;
    for (uint64_t i=q->head;i<q->tail;++i) {
        AuEntry *e=&q->entries[i % X4_STREAM_AU_ENTRIES];
        unsigned s=state(atomic_load_explicit(&e->tag,memory_order_acquire));
        if (s!=AU_READY && s!=AU_INFLIGHT && s!=AU_WRITING) continue;
        uint64_t age=now>=e->enqueued_us?now-e->enqueued_us:0;
        if (q->newest_timestamp_valid && e->timestamp_valid) {
            uint32_t ticks=q->newest_timestamp-e->timestamp;
            /* Only forward distances less than half the RTP32 modulus count.
             * Backward/ambiguous timestamps use local residence age. A
             * forward discontinuity deliberately errs toward recovery. */
            if (ticks<UINT32_C(0x80000000)) {
                uint64_t content=(uint64_t)ticks*1000000/90000;
                if (content>age) age=content;
            }
        }
        if (age>worst) worst=age;
    }
    return worst;
}
static uint64_t observe(X4StreamAuQueue *q, uint64_t now)
{
    uint64_t us=lag(q,now);unsigned depth=live_depth(q);
    ++q->samples;++q->depth_hist[depth];if(depth>q->depth_max)q->depth_max=depth;
    if(us>q->lag_max)q->lag_max=us;
    uint64_t bucket=us?((us-1)/HIST_QUANTUM_US)+1:0;
    if(bucket>=HIST_BUCKETS)++q->lag_overflow;else ++q->lag_hist[bucket];
    return us;
}
static uint64_t percentile(const uint64_t *hist, unsigned size, uint64_t total, unsigned percent)
{
    if(!total)return 0;
    uint64_t rank=total/100*percent+(total%100*percent+99)/100,sum=0;
    for(unsigned i=0;i<size;++i){sum+=hist[i];if(sum>=rank)return i;}
    return UINT64_MAX;
}
static void touch_zero_pages(void *memory,size_t bytes)
{
    /* Init owner only. Volatile writes cannot be discarded as dead memset.
     * Touch each4KiB span and the final byte, including an unaligned tail.
     * This removes allocation first-touch from AU admission; it is not mlock
     * or a guarantee about later OS paging/scheduling. */
    volatile unsigned char *p=memory;
    for(size_t offset=0;offset<bytes;offset+=4096)p[offset]=0;
    if(bytes)p[bytes-1]=0;
}
X4StreamAuQueue *x4_stream_au_queue_create(void)
{
    X4StreamAuQueue *q=calloc(1,sizeof(*q));if(!q)return NULL;
    /* calloc metadata is still all zero here, before pointer/atomic fields
     * are assigned. Its per-entry SPS/PPS pages are pre-touched as well. */
    touch_zero_pages(q,sizeof(*q));
    q->arena=malloc(X4_STREAM_AU_ARENA_BYTES);if(!q->arena){free(q);return NULL;}
    touch_zero_pages(q->arena,X4_STREAM_AU_ARENA_BYTES);
    atomic_flag_clear(&q->gate);atomic_init(&q->accepting,true);atomic_init(&q->fatal,false);
    atomic_init(&q->active_ops,0);atomic_init(&q->recovery,0);
#define INIT_COUNTER(f) atomic_init(&q->counters.f,0)
    INIT_COUNTER(admitted);INIT_COUNTER(taken);INIT_COUNTER(released);
    INIT_COUNTER(dropped_full);INIT_COUNTER(dropped_late);INIT_COUNTER(dropped_wait_idr);INIT_COUNTER(dropped_busy);
    INIT_COUNTER(rejected_incoming_full);INIT_COUNTER(rejected_incoming_late);INIT_COUNTER(rejected_incoming_wait_idr);INIT_COUNTER(rejected_incoming_busy);
    INIT_COUNTER(retired_ready_full);INIT_COUNTER(retired_ready_late);INIT_COUNTER(retired_ready_busy);
    INIT_COUNTER(evicted_non_idr);INIT_COUNTER(old_idr_retired);INIT_COUNTER(recovery_transitions);
    INIT_COUNTER(request_intentions);INIT_COUNTER(request_coalesced);
#undef INIT_COUNTER
    for(unsigned i=0;i<X4_STREAM_AU_ENTRIES;++i)atomic_init(&q->entries[i].tag,AU_FREE);
    if(!atomic_is_lock_free(&q->recovery)||!atomic_is_lock_free(&q->entries[0].tag)) {
        free(q->arena);free(q);return NULL;
    }
    return q;
}
static X4StreamAuResult done(X4StreamAuQueue *q,X4StreamAuOutcome *out,
    bool request,X4StreamAuResult rc,bool held)
{
    outcome(q,out,request);if(held)leave(q);
    atomic_fetch_sub_explicit(&q->active_ops,1,memory_order_release);return rc;
}
X4StreamAuResult x4_stream_au_queue_try_push(X4StreamAuQueue *q,
    const X4StreamAuInput *input,uint64_t now,X4StreamAuOutcome *out)
{
    if(!q)return X4_STREAM_AU_FATAL;
    atomic_fetch_add_explicit(&q->active_ops,1,memory_order_acquire);
    bool request=false;
    if(!running(q))return done(q,out,false,atomic_load(&q->fatal)?X4_STREAM_AU_FATAL:X4_STREAM_AU_STOPPED,false);
    if(!input||!input->data||!input->bytes||input->bytes>X4_STREAM_AU_MAX_BYTES||
        !input->sps||!input->sps_bytes||input->sps_bytes>X4_STREAM_AU_PARAMETER_BYTES||
        input->pps_bytes>X4_STREAM_AU_PARAMETER_BYTES||(input->pps_bytes&&!input->pps))
        return done(q,out,false,fatal(q),false);
    if(!enter(q)) {
        INC(q,dropped_busy);INC(q,rejected_incoming_busy);bool ok=recover(q,X4_STREAM_AU_LOSS_BUSY,&request);
        return done(q,out,request,ok?X4_STREAM_AU_BUSY:X4_STREAM_AU_FATAL,false);
    }
    HOOK(q,X4_STREAM_AU_AFTER_TRY_GATE);
    apply_recovery(q);
    q->newest_timestamp=input->rtp_timestamp;q->newest_timestamp_valid=input->rtp_timestamp_valid;
    uint64_t measured=observe(q,now);
    uint64_t token=atomic_load_explicit(&q->recovery,memory_order_acquire);
    if((token & 1) && !input->is_idr) {
        INC(q,dropped_wait_idr);INC(q,rejected_incoming_wait_idr);return done(q,out,false,X4_STREAM_AU_DROPPED_WAIT_IDR,true);
    }
    bool incoming_old=input->arrival_us && now>=input->arrival_us && now-input->arrival_us>X4_STREAM_AU_LAG_US;
    if(incoming_old || (measured>X4_STREAM_AU_LAG_US && !(token & 1))) {
        if(!recover(q,X4_STREAM_AU_LOSS_LATE,&request))return done(q,out,request,X4_STREAM_AU_FATAL,true);
        apply_recovery(q);
        if(!input->is_idr || incoming_old) {
            INC(q,dropped_late);INC(q,rejected_incoming_late);return done(q,out,request,X4_STREAM_AU_DROPPED_LATE,true);
        }
    }
    size_t physical=(size_t)(q->arena_tail % X4_STREAM_AU_ARENA_BYTES);
    size_t padding=input->bytes>X4_STREAM_AU_ARENA_BYTES-physical?X4_STREAM_AU_ARENA_BYTES-physical:0;
    if(q->tail-q->head==X4_STREAM_AU_ENTRIES ||
        q->arena_tail-q->arena_head+padding+input->bytes>X4_STREAM_AU_ARENA_BYTES) {
        if(!recover(q,X4_STREAM_AU_LOSS_FULL,&request))return done(q,out,request,X4_STREAM_AU_FATAL,true);
        apply_recovery(q);
        physical=(size_t)(q->arena_tail % X4_STREAM_AU_ARENA_BYTES);
        padding=input->bytes>X4_STREAM_AU_ARENA_BYTES-physical?X4_STREAM_AU_ARENA_BYTES-physical:0;
        if(!input->is_idr || q->tail-q->head==X4_STREAM_AU_ENTRIES ||
            q->arena_tail-q->arena_head+padding+input->bytes>X4_STREAM_AU_ARENA_BYTES) {
            INC(q,dropped_full);INC(q,rejected_incoming_full);return done(q,out,request,X4_STREAM_AU_DROPPED_FULL,true);
        }
    }
    if(q->next_generation==(UINT64_MAX >> TAG_BITS) || q->tail==UINT64_MAX ||
        q->arena_tail>UINT64_MAX-padding-input->bytes)
        return done(q,out,request,fatal(q),true);
    unsigned slot=(unsigned)(q->tail % X4_STREAM_AU_ENTRIES);AuEntry *e=&q->entries[slot];
    if(state(atomic_load_explicit(&e->tag,memory_order_acquire))!=AU_FREE)
        return done(q,out,request,fatal(q),true);
    token=atomic_load_explicit(&q->recovery,memory_order_acquire);
    uint64_t generation=++q->next_generation,writing=tag_for(generation,AU_WRITING);
    e->epoch=token>>RECOVERY_BITS;e->offset=padding?0:physical;e->bytes=input->bytes;
    e->sps_bytes=input->sps_bytes;e->pps_bytes=input->pps_bytes;e->pts_us=input->pts_us;
    e->identity=input->identity;e->arrival_us=input->arrival_us;e->enqueued_us=now;
    e->timestamp=input->rtp_timestamp;e->timestamp_valid=input->rtp_timestamp_valid;e->is_idr=input->is_idr;
    q->arena_tail+=padding+input->bytes;e->allocation_end=q->arena_tail;q->padding_total+=padding;++q->tail;
    atomic_store_explicit(&e->tag,writing,memory_order_release);
    unsigned reserved_depth=live_depth(q);
    if(reserved_depth>q->depth_max)q->depth_max=reserved_depth;
    leave(q);
    HOOK(q,X4_STREAM_AU_AFTER_RESERVE);
    memcpy(q->arena+e->offset,input->data,input->bytes);memcpy(e->sps,input->sps,input->sps_bytes);
    if(input->pps_bytes)memcpy(e->pps,input->pps,input->pps_bytes);
    HOOK(q,X4_STREAM_AU_BEFORE_PUBLISH);
    if(!running(q) || (atomic_load_explicit(&q->recovery,memory_order_acquire)>>RECOVERY_BITS)!=e->epoch) {
        atomic_store_explicit(&e->tag,tag_for(generation,AU_DONE),memory_order_release);
        if(!running(q))return done(q,out,request,X4_STREAM_AU_STOPPED,false);
        INC(q,dropped_wait_idr);INC(q,rejected_incoming_wait_idr);return done(q,out,request,X4_STREAM_AU_DROPPED_WAIT_IDR,false);
    }
    INC(q,admitted);
    HOOK(q,X4_STREAM_AU_BEFORE_READY_STORE);
    atomic_store_explicit(&e->tag,tag_for(generation,AU_READY),memory_order_release);
    if(input->is_idr) {
        uint64_t expected=token;
        (void)atomic_compare_exchange_strong_explicit(&q->recovery,&expected,token & ~UINT64_C(1),
            memory_order_acq_rel,memory_order_acquire);
    }
    return done(q,out,request,X4_STREAM_AU_OK,false);
}
X4StreamAuResult x4_stream_au_queue_try_take(X4StreamAuQueue *q,uint64_t now,
    X4StreamAuLease *lease,X4StreamAuView *view,X4StreamAuOutcome *out)
{
    if(!q||!lease||!view)return X4_STREAM_AU_FATAL;
    atomic_fetch_add_explicit(&q->active_ops,1,memory_order_acquire);bool request=false;
    if(!running(q))return done(q,out,false,atomic_load(&q->fatal)?X4_STREAM_AU_FATAL:X4_STREAM_AU_STOPPED,false);
    /* Empty polling is not queue mutation. Inspect atomic tags only, never
     * head/tail or entry metadata, before attempting the producer's gate.
     * This prevents an idle Decode owner from creating admission contention.
     * A publication racing this scan is observed on the next bounded poll. */
    bool ready=false;
    for(unsigned i=0;i<X4_STREAM_AU_ENTRIES;++i){
        unsigned observed=state(atomic_load_explicit(&q->entries[i].tag,memory_order_acquire));
        if(observed==AU_INFLIGHT)return done(q,out,false,X4_STREAM_AU_BUSY,false);
        if(observed==AU_READY)ready=true;
    }
    if(!ready)return done(q,out,false,X4_STREAM_AU_EMPTY,false);
    if(!enter(q))return done(q,out,false,X4_STREAM_AU_BUSY,false);
    apply_recovery(q);
    /* A stale READY could have been retired above. Do not let EMPTY polls
     * dominate the queue-delay percentile distribution. */
    uint64_t measured=live_depth(q)?observe(q,now):0;
    uint64_t token=atomic_load_explicit(&q->recovery,memory_order_acquire);
    if(measured>X4_STREAM_AU_LAG_US && !(token & 1)) {
        if(!recover(q,X4_STREAM_AU_LOSS_LATE,&request))return done(q,out,request,X4_STREAM_AU_FATAL,true);
        apply_recovery(q);INC(q,dropped_late);
        return done(q,out,request,X4_STREAM_AU_DROPPED_LATE,true);
    }
    for(uint64_t i=q->head;i<q->tail;++i)
        if(state(atomic_load_explicit(&q->entries[i % X4_STREAM_AU_ENTRIES].tag,memory_order_acquire))==AU_INFLIGHT)
            return done(q,out,false,X4_STREAM_AU_BUSY,true);
    token=atomic_load_explicit(&q->recovery,memory_order_acquire);
    for(uint64_t i=q->head;i<q->tail;++i) {
        unsigned slot=(unsigned)(i % X4_STREAM_AU_ENTRIES);AuEntry *e=&q->entries[slot];
        uint64_t tag=atomic_load_explicit(&e->tag,memory_order_acquire);
        if(state(tag)==AU_WRITING)break; /* An unpublished reservation preserves AU order. */
        if(state(tag)!=AU_READY)continue;
        if(e->epoch!=(token>>RECOVERY_BITS) || ((token & 1)&&!e->is_idr))continue;
        uint64_t claimed=tag_for(tag >> TAG_BITS,AU_INFLIGHT);
        HOOK(q,X4_STREAM_AU_BEFORE_TAKE_CLAIM);
        if(!atomic_compare_exchange_strong_explicit(&e->tag,&tag,claimed,memory_order_acq_rel,memory_order_acquire))
            return done(q,out,false,X4_STREAM_AU_BUSY,true);
        /* A loss racing between the token check and claim must not hand a
         * stale GOP to Decode. No view/lease has escaped yet. A loss after
         * this check observes a buffer already claimed by the consumer. */
        uint64_t after_claim=atomic_load_explicit(&q->recovery,memory_order_acquire);
        if((after_claim>>RECOVERY_BITS)!=e->epoch) {
            atomic_store_explicit(&e->tag,tag_for(claimed >> TAG_BITS,AU_DONE),memory_order_release);
            retired(q,e->is_idr,after_claim);
            apply_recovery(q);
            return done(q,out,false,X4_STREAM_AU_DROPPED_WAIT_IDR,true);
        }
        *lease=(X4StreamAuLease){claimed >> TAG_BITS,slot};
        *view=(X4StreamAuView){q->arena+e->offset,e->sps,e->pps,e->bytes,e->sps_bytes,e->pps_bytes,
            e->pts_us,e->identity,e->arrival_us,e->enqueued_us,e->timestamp,e->timestamp_valid,e->is_idr};
        INC(q,taken);return done(q,out,false,X4_STREAM_AU_OK,true);
    }
    return done(q,out,false,X4_STREAM_AU_EMPTY,true);
}
X4StreamAuResult x4_stream_au_queue_release(X4StreamAuQueue *q,X4StreamAuLease lease)
{
    if(!q||lease.slot>=X4_STREAM_AU_ENTRIES||!lease.generation||
        lease.generation>(UINT64_MAX >> TAG_BITS))return X4_STREAM_AU_FATAL;
    atomic_fetch_add_explicit(&q->active_ops,1,memory_order_acquire);
    uint64_t expected=tag_for(lease.generation,AU_INFLIGHT);
    bool released=atomic_compare_exchange_strong_explicit(&q->entries[lease.slot].tag,&expected,
        tag_for(lease.generation,AU_DONE),memory_order_release,memory_order_relaxed);
    if(released)INC(q,released);
    atomic_fetch_sub_explicit(&q->active_ops,1,memory_order_release);
    return released?X4_STREAM_AU_OK:X4_STREAM_AU_FATAL;
}
X4StreamAuResult x4_stream_au_queue_require_idr(X4StreamAuQueue *q,
    X4StreamAuLoss reason,X4StreamAuOutcome *out)
{
    if(!q||reason<X4_STREAM_AU_LOSS_FULL||reason>X4_STREAM_AU_LOSS_BUSY)return X4_STREAM_AU_FATAL;
    atomic_fetch_add_explicit(&q->active_ops,1,memory_order_acquire);bool request=false;
    if(!running(q))return done(q,out,false,X4_STREAM_AU_STOPPED,false);
    bool ok=recover(q,reason,&request);return done(q,out,request,ok?X4_STREAM_AU_OK:X4_STREAM_AU_FATAL,false);
}
X4StreamAuResult x4_stream_au_queue_try_snapshot(X4StreamAuQueue *q,X4StreamAuStats *out)
{
    if(!q||!out)return X4_STREAM_AU_FATAL;
    atomic_fetch_add_explicit(&q->active_ops,1,memory_order_acquire);
    if(!enter(q)){atomic_fetch_sub_explicit(&q->active_ops,1,memory_order_release);return X4_STREAM_AU_BUSY;}
    apply_recovery(q);X4StreamAuStats s={0};
#define COPY(f) s.f=VALUE(q,f)
    COPY(admitted);COPY(taken);COPY(released);COPY(dropped_full);COPY(dropped_late);COPY(dropped_wait_idr);COPY(dropped_busy);
    COPY(rejected_incoming_full);COPY(rejected_incoming_late);COPY(rejected_incoming_wait_idr);COPY(rejected_incoming_busy);
    COPY(retired_ready_full);COPY(retired_ready_late);COPY(retired_ready_busy);
    COPY(evicted_non_idr);COPY(old_idr_retired);COPY(recovery_transitions);COPY(request_intentions);COPY(request_coalesced);
#undef COPY
    s.padding_bytes_total=q->padding_total;s.samples=q->samples;
    uint64_t p50=percentile(q->lag_hist,HIST_BUCKETS,q->samples,50),p95=percentile(q->lag_hist,HIST_BUCKETS,q->samples,95);
    s.lag_p50_upper_us=p50==UINT64_MAX?UINT64_MAX:p50*HIST_QUANTUM_US;
    s.lag_p95_upper_us=p95==UINT64_MAX?UINT64_MAX:p95*HIST_QUANTUM_US;s.lag_max_us=q->lag_max;s.lag_histogram_overflow=q->lag_overflow;
    s.depth_p50=(unsigned)percentile(q->depth_hist,X4_STREAM_AU_ENTRIES+1,q->samples,50);
    s.depth_p95=(unsigned)percentile(q->depth_hist,X4_STREAM_AU_ENTRIES+1,q->samples,95);s.depth_max=q->depth_max;
    s.resident_entries=(unsigned)(q->tail-q->head);s.resident_bytes=(size_t)(q->arena_tail-q->arena_head);
    for(uint64_t i=q->head;i<q->tail;++i){AuEntry *e=&q->entries[i % X4_STREAM_AU_ENTRIES];
        unsigned st=state(atomic_load_explicit(&e->tag,memory_order_acquire));
        if(st==AU_READY)++s.ready_entries;
        if(st==AU_INFLIGHT)++s.inflight_entries;
        if(st==AU_READY||st==AU_WRITING||st==AU_INFLIGHT)s.payload_bytes+=e->bytes;}
    s.arena_bytes=X4_STREAM_AU_ARENA_BYTES;s.metadata_bytes=sizeof(*q);
    s.waiting_idr=(atomic_load_explicit(&q->recovery,memory_order_acquire)&1)!=0;
    s.stopped=!atomic_load_explicit(&q->accepting,memory_order_acquire);s.fatal=atomic_load_explicit(&q->fatal,memory_order_acquire);
    *out=s;leave(q);atomic_fetch_sub_explicit(&q->active_ops,1,memory_order_release);return X4_STREAM_AU_OK;
}
void x4_stream_au_queue_stop(X4StreamAuQueue *q)
{if(q)atomic_store_explicit(&q->accepting,false,memory_order_release);}
bool x4_stream_au_queue_quiescent(const X4StreamAuQueue *q)
{
    if(!q)return true;
    if(atomic_load_explicit(&q->accepting,memory_order_acquire)||atomic_load_explicit(&q->active_ops,memory_order_acquire))return false;
    for(unsigned i=0;i<X4_STREAM_AU_ENTRIES;++i){unsigned st=state(atomic_load_explicit(&q->entries[i].tag,memory_order_acquire));
        if(st==AU_WRITING||st==AU_INFLIGHT)return false;}
    return true;
}
bool x4_stream_au_queue_destroy(X4StreamAuQueue *q)
{
    if(!q)return true;
    if(!x4_stream_au_queue_quiescent(q))return false;
    free(q->arena);free(q);return true;
}
