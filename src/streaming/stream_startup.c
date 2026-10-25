/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_startup.h"
#include <string.h>

void x4_stream_startup_init(X4StreamStartup *s,bool enabled)
{
    if(!s)return;
    memset(s,0,sizeof(*s));s->enabled=enabled;
    atomic_init(&s->alias_sent_us,0);atomic_init(&s->alias_rc,0);
    atomic_init(&s->alias_attempted,false);atomic_init(&s->waiting,enabled);
}
void x4_stream_startup_note_alias(X4StreamStartup *s,int rc,uint64_t now)
{
    if(!s||!s->enabled)return;
    atomic_store_explicit(&s->alias_rc,rc,memory_order_relaxed);
    atomic_store_explicit(&s->alias_attempted,true,memory_order_release);
    if(rc>=0&&now){uint64_t empty=0;
        (void)atomic_compare_exchange_strong_explicit(&s->alias_sent_us,&empty,now,
            memory_order_release,memory_order_relaxed);}
}
uint64_t x4_stream_startup_alias_sent_us(const X4StreamStartup *s)
{return s?atomic_load_explicit(&s->alias_sent_us,memory_order_acquire):0;}
int x4_stream_startup_alias_result(const X4StreamStartup *s,bool *attempted)
{
    bool seen=s&&atomic_load_explicit(&s->alias_attempted,memory_order_acquire);
    if(attempted)*attempted=seen;
    return seen?atomic_load_explicit(&s->alias_rc,memory_order_relaxed):0;
}
bool x4_stream_startup_waiting(const X4StreamStartup *s)
{return s&&atomic_load_explicit(&s->waiting,memory_order_acquire);}
int x4_stream_startup_poll(X4StreamStartup *s,uint64_t now,bool *request)
{
    if(request)*request=false;
    if(!s||!s->enabled||s->admitted||s->cancelled)return 0;
    if(s->timed_out)return X4_STREAM_INITIAL_RESOLUTION_TIMEOUT;
    uint64_t sent=x4_stream_startup_alias_sent_us(s);
    if(!sent||now<sent)return 0;
    if(now-sent>=X4_STREAM_INITIAL_RESOLUTION_WAIT_US){
        s->timed_out=true;atomic_store_explicit(&s->waiting,false,memory_order_release);
        return X4_STREAM_INITIAL_RESOLUTION_TIMEOUT;
    }
    if(!s->last_request_us||now-s->last_request_us>=X4_STREAM_INITIAL_RESOLUTION_RETRY_US){
        s->last_request_us=now;if(request)*request=true;
    }
    return 0;
}
bool x4_stream_startup_hold_size(X4StreamStartup *s,unsigned width,unsigned height)
{
    if(!s||!s->enabled||s->admitted||s->cancelled||s->timed_out)return false;
    s->last_width=width;s->last_height=height;++s->dropped_aus;return true;
}
void x4_stream_startup_admit(X4StreamStartup *s,uint64_t now)
{
    if(!s||!s->enabled||s->admitted||s->cancelled||s->timed_out)return;
    s->admitted=true;s->first_admitted_us=now;
    uint64_t sent=x4_stream_startup_alias_sent_us(s);
    s->elapsed_available=sent&&now>=sent;s->alias_to_sps_us=s->elapsed_available?now-sent:0;
    atomic_store_explicit(&s->waiting,false,memory_order_release);
}
void x4_stream_startup_cancel(X4StreamStartup *s)
{if(s){s->cancelled=true;atomic_store_explicit(&s->waiting,false,memory_order_release);}}
