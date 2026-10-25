/* SPDX-License-Identifier: GPL-3.0-only */
#include "keyframe_limiter.h"
#include <string.h>

static uint64_t increment(X4KeyframeLimiter *l, atomic_uint_fast64_t *counter)
{
    uint_fast64_t old=atomic_fetch_add_explicit(counter,1,memory_order_relaxed);
    if(old==UINT_FAST64_MAX)
        atomic_store_explicit(&l->counter_overflow,true,memory_order_release);
    return (uint64_t)(old+1);
}

void x4_keyframe_limiter_init(X4KeyframeLimiter *l)
{
    if(!l)return;
    memset(l,0,sizeof(*l));
#define INIT(name) atomic_init(&l->name,0)
    INIT(pending_reasons);INIT(requested);INIT(coalesced);INIT(deferred);INIT(granted);INIT(retired_on_idr);
    INIT(interval_deferred);INIT(cap_deferred);INIT(clock_deferred);
    INIT(gap_generation);INIT(idr_generation);INIT(granted_gap_generation);INIT(last_granted_us);
    INIT(transport_decisions);INIT(control_attempts);INIT(control_local_successes);
    INIT(control_errors);INIT(pli_attempts);INIT(pli_local_successes);INIT(pli_errors);
#undef INIT
    atomic_init(&l->counter_overflow,false);
}

void x4_keyframe_limiter_set_observer(X4KeyframeLimiter *l,
    void (*observer)(void *,uint64_t),void *user)
{if(l){l->observer=observer;l->observer_user=user;}}

void x4_keyframe_limiter_request(X4KeyframeLimiter *l,unsigned reasons)
{
    if(!l||!reasons||(reasons&~X4_KEYFRAME_ALL_REASONS))return;
    increment(l,&l->requested);
    if(atomic_fetch_or_explicit(&l->pending_reasons,reasons,memory_order_release))
        increment(l,&l->coalesced);
}

void x4_keyframe_limiter_observe_gap(X4KeyframeLimiter *l)
{if(l)increment(l,&l->gap_generation);}

void x4_keyframe_limiter_observe_idr(X4KeyframeLimiter *l)
{
    if(!l)return;
    uint64_t before=atomic_load_explicit(&l->gap_generation,memory_order_acquire);
    uint64_t served=atomic_load_explicit(&l->granted_gap_generation,memory_order_acquire);
    unsigned old=atomic_exchange_explicit(&l->pending_reasons,0,memory_order_acq_rel);
    uint64_t after=atomic_load_explicit(&l->gap_generation,memory_order_acquire);
    if(old){
        /* A useful IDR cannot prove coverage of new local damage. Preserve it
         * conservatively, including gap publication racing this exchange.
         * A request published after the exchange already remains pending. */
        if(before>served||after>served||atomic_load_explicit(&l->counter_overflow,memory_order_acquire)){
            if(atomic_fetch_or_explicit(&l->pending_reasons,old,memory_order_acq_rel))
                increment(l,&l->coalesced);
        }else increment(l,&l->retired_on_idr);
    }
    increment(l,&l->idr_generation);
}

static bool defer(X4KeyframeLimiter *l,atomic_uint_fast64_t *reason)
{
    increment(l,&l->deferred);increment(l,reason);return false;
}

bool x4_keyframe_limiter_take(X4KeyframeLimiter *l,uint64_t now,X4KeyframeGrant *out)
{
    if(out)*out=(X4KeyframeGrant){0};
    if(!l||!atomic_load_explicit(&l->pending_reasons,memory_order_acquire))return false;
    if(l->owner_has_grant&&now<l->owner_last_us)
        return defer(l,&l->clock_deferred);
    while(l->owner_count){
        uint64_t oldest=l->owner_times[l->owner_head];
        if(now<oldest||now-oldest<X4_KEYFRAME_WINDOW_US)break;
        l->owner_head=(l->owner_head+1)%X4_KEYFRAME_WINDOW_LIMIT;--l->owner_count;
    }
    if(l->owner_count==X4_KEYFRAME_WINDOW_LIMIT)
        return defer(l,&l->cap_deferred);
    uint64_t gap=atomic_load_explicit(&l->gap_generation,memory_order_acquire);
    uint64_t idr=atomic_load_explicit(&l->idr_generation,memory_order_acquire);
    bool fresh=(gap>l->owner_gap_generation||idr>l->owner_idr_generation)&&
        !atomic_load_explicit(&l->counter_overflow,memory_order_acquire);
    bool early=l->owner_has_grant&&now-l->owner_last_us<X4_KEYFRAME_INTERVAL_US;
    if(early&&!fresh)return defer(l,&l->interval_deferred);

    /* This exchange consumes only intents published before this reservation.
     * A producer posting afterwards leaves work pending for a later take.
     * Capture evidence again at this boundary: evidence already published by
     * then cannot incorrectly buy another early reservation for this grant. */
    unsigned reasons=atomic_exchange_explicit(&l->pending_reasons,0,memory_order_acq_rel);
    if(!reasons)return false; /* Only the documented single owner may take. */
    l->owner_gap_generation=atomic_load_explicit(&l->gap_generation,memory_order_acquire);
    l->owner_idr_generation=atomic_load_explicit(&l->idr_generation,memory_order_acquire);
    unsigned at=(l->owner_head+l->owner_count)%X4_KEYFRAME_WINDOW_LIMIT;
    l->owner_times[at]=now;++l->owner_count;
    l->owner_last_us=now;l->owner_has_grant=true;
    atomic_store_explicit(&l->granted_gap_generation,l->owner_gap_generation,memory_order_release);
    atomic_store_explicit(&l->last_granted_us,now,memory_order_release);
    uint64_t sequence=increment(l,&l->granted);
    if(out)*out=(X4KeyframeGrant){.sequence=sequence,.at_us=now,
        .gap_generation=l->owner_gap_generation,.idr_generation=l->owner_idr_generation,
        .reasons=reasons,.exception=early};
    if(l->observer)l->observer(l->observer_user,now);
    return true;
}

void x4_keyframe_limiter_note_transport(X4KeyframeLimiter *l,
    bool control_attempted,int control_rc,bool pli_attempted,int pli_rc)
{
    if(!l||(!control_attempted&&!pli_attempted))return;
    increment(l,&l->transport_decisions);
    if(control_attempted){
        increment(l,&l->control_attempts);
        increment(l,control_rc>=0?&l->control_local_successes:&l->control_errors);
    }
    if(pli_attempted){
        increment(l,&l->pli_attempts);
        increment(l,pli_rc>=0?&l->pli_local_successes:&l->pli_errors);
    }
}

void x4_keyframe_limiter_snapshot(const X4KeyframeLimiter *l,X4KeyframeLimiterSnapshot *out)
{
    if(!out)return;
    *out=(X4KeyframeLimiterSnapshot){0};if(!l)return;
#define READ(name) out->name=(uint64_t)atomic_load_explicit(&l->name,memory_order_acquire)
    READ(requested);READ(coalesced);READ(deferred);READ(granted);READ(retired_on_idr);
    READ(interval_deferred);READ(cap_deferred);READ(clock_deferred);
    READ(gap_generation);READ(idr_generation);READ(granted_gap_generation);READ(last_granted_us);
    READ(transport_decisions);READ(control_attempts);READ(control_local_successes);
    READ(control_errors);READ(pli_attempts);READ(pli_local_successes);READ(pli_errors);
#undef READ
    out->pending_reasons=atomic_load_explicit(&l->pending_reasons,memory_order_acquire);
    out->counter_overflow=atomic_load_explicit(&l->counter_overflow,memory_order_acquire);
}
