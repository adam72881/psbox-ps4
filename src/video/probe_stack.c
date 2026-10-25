/* SPDX-License-Identifier: GPL-3.0-only */
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
#if X4_PROBE_STACK_HOST
#define _GNU_SOURCE 1
#endif
#include "probe_stack.h"
#include <stdio.h>
#include <string.h>
#if X4_PROBE_STACK_HOST
#include <pthread.h>
static unsigned fail_step; static int fail_result;
void x4_probe_stack_test_fail(unsigned step, int result) { fail_step = step; fail_result = result; }
static int injected(unsigned step) { return fail_step == step ? fail_result : 0; }
#define ATTR_INIT(a) (injected(1) ? injected(1) : pthread_attr_init(a))
#define ATTR_SET(a,n) (injected(2) ? injected(2) : pthread_attr_setstacksize(a,n))
#define ATTR_DESTROY(a) (injected(5) ? (pthread_attr_destroy(a),injected(5)) : pthread_attr_destroy(a))
#define ATTR_GET(a) (injected(4) ? injected(4) : pthread_getattr_np(pthread_self(),a))
static int host_size(const pthread_attr_t *a,size_t *n)
{
    int rc=pthread_attr_getstacksize(a,n);
    if(!rc && fail_step==6) *n-=4096;
    return rc;
}
static int host_stack(const pthread_attr_t *a,void **p,size_t *n)
{
    int rc=pthread_attr_getstack(a,p,n);
    if(!rc && fail_step==8) *p=(void *)((uintptr_t)*p+*n);
    return rc;
}
static int host_guard(const pthread_attr_t *a,size_t *n)
{
    if(fail_step==7) return pthread_attr_getstacksize(a,n);
    return pthread_attr_getguardsize(a,n);
}
#define ATTR_SIZE(a,n) host_size(a,n)
#define ATTR_STACK(a,p,n) host_stack(a,p,n)
#define ATTR_GUARD(a,n) host_guard(a,n)
#define CREATE(t,a,f,p,n) (injected(3) ? injected(3) : scePthreadCreate(t,a,f,p,n))
#else
#define ATTR_INIT(a) scePthreadAttrInit(a)
#define ATTR_SET(a,n) scePthreadAttrSetstacksize(a,n)
#define ATTR_DESTROY(a) scePthreadAttrDestroy(a)
#define ATTR_GET(a) scePthreadAttrGet(scePthreadSelf(),a)
#define ATTR_SIZE(a,n) scePthreadAttrGetstacksize(a,n)
#define ATTR_STACK(a,p,n) scePthreadAttrGetstack(a,p,n)
#define ATTR_GUARD(a,n) scePthreadAttrGetguardsize(a,n)
#define CREATE(t,a,f,p,n) scePthreadCreate(t,a,f,p,n)
#endif
#if defined(__clang__)
#define RAW_STACK __attribute__((noinline,no_sanitize("address","undefined","thread")))
#else
#define RAW_STACK __attribute__((noinline,no_sanitize_address,no_sanitize_undefined,no_sanitize_thread))
#endif
static int negative(int rc) { return rc < 0 ? rc : -rc; }
static void report_init(X4ProbeStackReport *r, const char *name, size_t configured)
{
    memset(r,0,sizeof(*r)); snprintf(r->name,sizeof(r->name),"%s",name);
    r->configured_bytes=configured;
    r->attr_init_rc=r->set_size_rc=r->create_rc=r->attr_destroy_rc=X4_PROBE_STACK_NOT_RUN;
    r->getattr_init_rc=r->getattr_rc=r->getstacksize_rc=r->getstack_rc=X4_PROBE_STACK_NOT_RUN;
    r->getguardsize_rc=r->query_destroy_rc=r->measure_rc=r->join_rc=r->wrapper_result=X4_PROBE_STACK_NOT_RUN;
}
static __attribute__((noinline)) int query(X4ProbeStackReport *r, uintptr_t *low, uintptr_t *high)
{
    OrbisPthreadAttr attr;
    r->getattr_init_rc=ATTR_INIT(&attr);
    if(r->getattr_init_rc) return negative(r->getattr_init_rc);
    r->getattr_rc=ATTR_GET(&attr);
    size_t bytes=0, extent=0, guard=0; void *base=NULL;
    if(!r->getattr_rc) {
        r->getstacksize_rc=ATTR_SIZE(&attr,&bytes);
        r->getstack_rc=ATTR_STACK(&attr,&base,&extent);
        r->getguardsize_rc=ATTR_GUARD(&attr,&guard);
    }
    r->query_destroy_rc=ATTR_DESTROY(&attr);
    r->reported_bytes=bytes; r->guard_bytes=guard;
    int rc=r->getattr_rc ? r->getattr_rc : r->getstacksize_rc ? r->getstacksize_rc :
        r->getstack_rc ? r->getstack_rc : r->getguardsize_rc ? r->getguardsize_rc : r->query_destroy_rc;
    if(rc) return negative(rc);
    if(!base || !bytes || bytes!=extent || (uintptr_t)base>UINTPTR_MAX-bytes ||
        guard>=bytes || (r->configured_bytes && bytes!=r->configured_bytes)) return X4_PROBE_STACK_LAYOUT;
    *low=(uintptr_t)base; *high=*low+bytes; return 0;
}
void x4_probe_stack_query_current(X4ProbeStackReport *r)
{
    if(!r) return;
    report_init(r,"principal",0);
    uintptr_t low=0,high=0;
    r->measure_rc=query(r,&low,&high); /* Main is observed, never painted. */
}
/* A leaf volatile-byte loop intentionally has no calls, memset, sanitizer
 * callbacks or automatic arrays. Native x86-64 stacks grow down. The live
 * frame/red-zone and high-address runtime/TLS area are outside this interval.
 * The low reserve excludes both the reported guard and an extra native page.
 * Coverage starts AFTER startup attribute queries and ends BEFORE exit/join.
 * Equal-to-pattern writes and code outside the sampled interval are not seen:
 * this is a sampled lower-bound, never a proven exact peak/capacity guarantee. */
static RAW_STACK int paint(X4ProbeThread *t)
{
    uintptr_t sp;
    __asm__ volatile("mov %%rsp,%0" : "=r"(sp));
    uintptr_t low=t->stack_low, high=t->stack_high;
    size_t reserve=t->report.guard_bytes>X4_PROBE_STACK_LOW_RESERVE ?
        (size_t)t->report.guard_bytes : X4_PROBE_STACK_LOW_RESERVE;
    if(low>=high || reserve>=high-low || sp<=low+reserve || sp>=high ||
        sp-low-reserve<=X4_PROBE_STACK_TOP_RESERVE) return X4_PROBE_STACK_LAYOUT;
    t->report.entry_used_bytes=high-sp;
    t->paint_low=low+reserve; t->paint_high=sp-X4_PROBE_STACK_TOP_RESERVE;
    for(volatile unsigned char *p=(volatile unsigned char *)t->paint_low;
        (uintptr_t)p<t->paint_high;++p) *p=0xa7;
    t->report.low_excluded_bytes=reserve;
    t->report.top_excluded_bytes=high-t->paint_high;
    t->report.painted_bytes=t->paint_high-t->paint_low;
    t->report.pattern_ready=true; return 0;
}
static RAW_STACK void finish(X4ProbeThread *t)
{
    if(!t->report.pattern_ready) return;
    uintptr_t p=t->paint_low;
    while(p<t->paint_high && *(volatile unsigned char *)p==0xa7) ++p;
    t->report.untouched_bytes=p-t->paint_low;
    t->report.pattern_touched=p<t->paint_high;
    t->report.high_water_bytes=p<t->paint_high ? t->stack_high-p : t->report.entry_used_bytes;
    t->report.finished=true;
}
static __attribute__((noinline)) void *x4_probe_thread_trampoline(void *arg)
{
    X4ProbeThread *t=arg;
    while(!atomic_load_explicit(&t->command,memory_order_acquire)) sceKernelUsleep(1000);
    if(atomic_load_explicit(&t->command,memory_order_acquire)==3) return NULL;
#if X4_PROBE_STACK_HOST
    /* Deterministic host-only late query: creator times out first. */
    if(fail_step==9)
        while(atomic_load_explicit(&t->command,memory_order_acquire)!=3) sceKernelUsleep(1000);
#endif
    int rc=query(&t->report,&t->stack_low,&t->stack_high);
    if(!rc) rc=paint(t);
    t->report.measure_rc=rc;
    atomic_store_explicit(&t->startup_result,rc,memory_order_relaxed);
    atomic_store_explicit(&t->ready,true,memory_order_release);
    while(atomic_load_explicit(&t->command,memory_order_acquire)==1) sceKernelUsleep(1000);
    if(rc || atomic_load_explicit(&t->command,memory_order_acquire)!=2) return NULL;
    t->report.started=true; void *result=t->entry(t->argument); finish(t); return result;
}
int x4_probe_thread_create(OrbisPthread *id, X4ProbeThread *t,
    void *(*entry)(void *), void *arg, const char *name)
{
    if(!id || !t || !entry || !name) return X4_PROBE_STACK_LAYOUT;
    if(t->created) return X4_PROBE_STACK_RETAINED;
    memset(t,0,sizeof(*t)); report_init(&t->report,name,X4_PROBE_THREAD_STACK_BYTES);
    atomic_init(&t->command,0); atomic_init(&t->startup_result,X4_PROBE_STACK_NOT_RUN); atomic_init(&t->ready,false);
    t->entry=entry; t->argument=arg;
    OrbisPthreadAttr attr;
    t->report.attr_init_rc=ATTR_INIT(&attr);
    if(t->report.attr_init_rc) {
        t->report.wrapper_result=negative(t->report.attr_init_rc);
        return t->report.wrapper_result;
    }
    t->report.set_size_rc=ATTR_SET(&attr,X4_PROBE_THREAD_STACK_BYTES);
    if(!t->report.set_size_rc) t->report.create_rc=CREATE(id,&attr,x4_probe_thread_trampoline,t,name);
    t->created=t->report.create_rc==0;
    t->report.attr_destroy_rc=ATTR_DESTROY(&attr);
    int rc=t->report.set_size_rc ? t->report.set_size_rc : t->report.create_rc ? t->report.create_rc : t->report.attr_destroy_rc;
    if(rc) {
        t->report.wrapper_result=negative(rc);
        if(t->created) atomic_store_explicit(&t->command,3,memory_order_release);
        return t->report.wrapper_result; /* Caller must join created threads even on failure. */
    }
    atomic_store_explicit(&t->command,1,memory_order_release);
    unsigned waits=0;
#if X4_PROBE_STACK_HOST && defined(X4_PROBE_STACK_TEST_WAIT_LIMIT)
    const unsigned wait_limit=X4_PROBE_STACK_TEST_WAIT_LIMIT;
#else
    const unsigned wait_limit=2000; /* Sleep-attempt bound, not a wall-clock guarantee. */
#endif
    while(!atomic_load_explicit(&t->ready,memory_order_acquire) && waits++<wait_limit) sceKernelUsleep(1000);
    rc=atomic_load_explicit(&t->ready,memory_order_acquire) ? atomic_load(&t->startup_result) : X4_PROBE_STACK_TIMEOUT;
    t->report.wrapper_result=rc;
    atomic_store_explicit(&t->command,rc ? 3 : 2,memory_order_release);
    return rc;
}
void x4_probe_thread_joined(X4ProbeThread *t,int result)
{ if(t) { t->report.join_rc=result; if(!result) t->created=false; } }
static void *probe_stack_test_entry(void *arg) { (void)arg; return NULL; }
int x4_probe_stack_check(X4ProbeStackReport *main_report,X4ProbeStackReport *test_report)
{
    static X4ProbeThread t; static OrbisPthread id;
    if(!main_report || !test_report) return X4_PROBE_STACK_LAYOUT;
    x4_probe_stack_query_current(main_report);
    if(t.created) { report_init(test_report,"x4-stack-check",X4_PROBE_THREAD_STACK_BYTES);
        test_report->measure_rc=X4_PROBE_STACK_RETAINED; return X4_PROBE_STACK_RETAINED; }
    int rc=x4_probe_thread_create(&id,&t,probe_stack_test_entry,NULL,"x4-stack-check");
    if(t.created) {
        int joined=scePthreadJoin(id,NULL); x4_probe_thread_joined(&t,joined);
        if(joined) {
            report_init(test_report,"x4-stack-check",X4_PROBE_THREAD_STACK_BYTES);
            test_report->join_rc=joined; test_report->measure_rc=X4_PROBE_STACK_RETAINED;
            return negative(joined); /* Never race a still-live worker's report. */
        }
    }
    *test_report=t.report; return rc;
}
/* This small diagnostic thread deliberately uses the normal app's NULL
 * attributes. It queries itself and returns without touching unused stack.
 * The storage outlives every native thread, including an unsuccessful join. */
typedef struct {
    X4ProbeStackReport report;
    OrbisPthread id;
    bool created;
} X4ProbeDefaultStackCheck;
static void *probe_default_stack_entry(void *argument)
{
    X4ProbeDefaultStackCheck *t=argument;
    uintptr_t low=0,high=0;
    t->report.started=true;
    t->report.measure_rc=query(&t->report,&low,&high);
    t->report.wrapper_result=t->report.measure_rc;
    t->report.finished=true;
    return NULL;
}
int x4_probe_default_stack_check(X4ProbeStackReport *report,bool *retained)
{
    static X4ProbeDefaultStackCheck t;
    if(!report || !retained) return X4_PROBE_STACK_LAYOUT;
    *retained=false;
    if(!t.created) {
        report_init(&t.report,"x4-stack-default",0);
        t.report.create_rc=CREATE(&t.id,NULL,probe_default_stack_entry,&t,"x4-stack-default");
        t.created=t.report.create_rc==0;
        if(!t.created) {
            t.report.wrapper_result=negative(t.report.create_rc);
            *report=t.report;
            return report->wrapper_result;
        }
    }
    int joined=scePthreadJoin(t.id,NULL);
    if(joined) {
        /* Do not snapshot a report the thread may still be changing. All
         * fields exposed here are known locally, independent of its progress. */
        *retained=true;
        report_init(report,"x4-stack-default",0);
        report->create_rc=0;
        report->join_rc=joined;
        report->measure_rc=X4_PROBE_STACK_RETAINED;
        report->wrapper_result=negative(joined);
        return report->wrapper_result;
    }
    t.created=false;
    t.report.join_rc=0;
    *report=t.report;
    return report->wrapper_result;
}
#endif
