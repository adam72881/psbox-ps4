/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "../src/video/probe_stack.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned checks, calls, default_creates;
static int fail_join;
static size_t default_expected_size, default_expected_guard;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);abort();} } while(0)
int sceKernelUsleep(unsigned us)
{ struct timespec t={us/1000000u,(long)(us%1000000u)*1000}; while(nanosleep(&t,&t)&&errno==EINTR){} return 0; }
int scePthreadCreate(OrbisPthread *t,const pthread_attr_t *a,void *(*entry)(void *),void *arg,const char *name)
{
    if(!a) {
        CHECK(!strcmp(name,"x4-stack-default")); ++default_creates;
        pthread_attr_t defaults;
        CHECK(!pthread_attr_init(&defaults));
        CHECK(!pthread_attr_getstacksize(&defaults,&default_expected_size));
        CHECK(!pthread_attr_getguardsize(&defaults,&default_expected_guard));
        CHECK(!pthread_attr_destroy(&defaults));
    }
    else { size_t size=0; CHECK(!pthread_attr_getstacksize(a,&size)); CHECK(size==X4_PROBE_THREAD_STACK_BYTES); }
    return pthread_create(t,a,entry,arg);
}
int scePthreadJoin(OrbisPthread t,void **out) { return fail_join ? fail_join : pthread_join(t,out); }
static __attribute__((noinline)) unsigned deep3(unsigned seed)
{ volatile unsigned char data[6144];for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)(seed+i);return data[seed%sizeof(data)]; }
static __attribute__((noinline)) unsigned deep2(unsigned seed)
{ volatile unsigned char data[6144];for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)(seed+i);return deep3(seed)+data[seed%sizeof(data)]; }
static __attribute__((noinline)) unsigned deep1(unsigned seed)
{ volatile unsigned char data[6144];for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)(seed+i);return deep2(seed)+data[seed%sizeof(data)]; }
static void *work(void *arg) { ++calls; unsigned *result=arg;*result=deep1(29);return NULL; }
static void join_thread(OrbisPthread id,X4ProbeThread *t)
{ int rc=scePthreadJoin(id,NULL);x4_probe_thread_joined(t,rc);CHECK(!rc);CHECK(!t->created); }
int main(void)
{
    X4ProbeThread *t=calloc(1,sizeof(*t));CHECK(t); OrbisPthread id;unsigned result=0;
    CHECK(!x4_probe_thread_create(&id,t,work,&result,"x4-test"));
    CHECK(x4_probe_thread_create(&id,t,work,&result,"duplicate")==X4_PROBE_STACK_RETAINED);
    join_thread(id,t);CHECK(calls==1);CHECK(result!=0);CHECK(t->report.started&&t->report.finished);
    CHECK(t->report.configured_bytes==262144&&t->report.reported_bytes==262144);
    CHECK(t->report.attr_init_rc==0&&t->report.set_size_rc==0&&t->report.create_rc==0&&t->report.attr_destroy_rc==0);
    CHECK(t->report.getattr_init_rc==0&&t->report.getattr_rc==0&&t->report.getstacksize_rc==0&&t->report.getstack_rc==0);
    CHECK(t->report.getguardsize_rc==0&&t->report.query_destroy_rc==0&&t->report.measure_rc==0&&t->report.join_rc==0);
    CHECK(t->report.wrapper_result==0);
    CHECK(t->report.pattern_ready&&t->report.pattern_touched);
    CHECK(t->report.high_water_bytes>=12288&&t->report.high_water_bytes<131072);
    CHECK(t->report.untouched_bytes<t->report.painted_bytes);
    CHECK(t->report.painted_bytes+t->report.low_excluded_bytes+t->report.top_excluded_bytes==262144);
    CHECK(t->report.top_excluded_bytes==t->report.entry_used_bytes+X4_PROBE_STACK_TOP_RESERVE);
    printf("HOST stack=%llu high_water_sample=%llu painted=%llu entry=%llu\n",
        (unsigned long long)t->report.reported_bytes,(unsigned long long)t->report.high_water_bytes,
        (unsigned long long)t->report.painted_bytes,(unsigned long long)t->report.entry_used_bytes);
    for(unsigned fail=1;fail<=5;++fail) {
        unsigned before=calls;x4_probe_stack_test_fail(fail,35);
        int rc=x4_probe_thread_create(&id,t,work,&result,"x4-fail");CHECK(rc==-35);
        if(t->created)join_thread(id,t);
        CHECK(calls==before);CHECK(!t->report.started);CHECK(!t->created);
    }
    for(unsigned fail=6;fail<=8;++fail) {
        unsigned before=calls;x4_probe_stack_test_fail(fail,0);
        CHECK(x4_probe_thread_create(&id,t,work,&result,"x4-geometry")==X4_PROBE_STACK_LAYOUT);
        CHECK(t->created);join_thread(id,t);CHECK(calls==before);
        CHECK(!t->report.started&&!t->report.pattern_ready);
    }
    x4_probe_stack_test_fail(9,0);
    CHECK(x4_probe_thread_create(&id,t,work,&result,"x4-timeout")==X4_PROBE_STACK_TIMEOUT);
    CHECK(t->created);join_thread(id,t);
    CHECK(t->report.wrapper_result==X4_PROBE_STACK_TIMEOUT&&t->report.measure_rc==0);
    CHECK(!t->report.started&&calls==1);
    x4_probe_stack_test_fail(0,0);
    CHECK(!x4_probe_thread_create(&id,t,work,&result,"x4-retry"));join_thread(id,t);CHECK(calls==2);
    X4ProbeStackReport main_report,test_report;
    CHECK(!x4_probe_stack_check(&main_report,&test_report));CHECK(main_report.reported_bytes>0);
    CHECK(main_report.configured_bytes==0&&!main_report.pattern_ready);
    CHECK(test_report.reported_bytes==262144&&test_report.finished&&test_report.join_rc==0);
    CHECK(!test_report.pattern_touched || test_report.high_water_bytes>=test_report.entry_used_bytes);
    x4_probe_stack_test_fail(2,22);CHECK(x4_probe_stack_check(&main_report,&test_report)==-22);
    CHECK(test_report.set_size_rc==22&&test_report.create_rc==X4_PROBE_STACK_NOT_RUN);
    x4_probe_stack_test_fail(0,0);
    X4ProbeStackReport default_report; bool retained=true;
    CHECK(!x4_probe_default_stack_check(&default_report,&retained)); CHECK(!retained);
    CHECK(default_creates==1 && default_report.configured_bytes==0);
    CHECK(default_report.reported_bytes>0 && default_report.guard_bytes<default_report.reported_bytes);
    CHECK(default_report.reported_bytes==default_expected_size && default_report.guard_bytes==default_expected_guard);
    CHECK(default_report.started && default_report.finished && !default_report.pattern_ready);
    CHECK(default_report.painted_bytes==0 && default_report.high_water_bytes==0);
    CHECK(default_report.attr_init_rc==X4_PROBE_STACK_NOT_RUN);
    CHECK(default_report.set_size_rc==X4_PROBE_STACK_NOT_RUN);
    CHECK(default_report.attr_destroy_rc==X4_PROBE_STACK_NOT_RUN);
    CHECK(default_report.create_rc==0 && default_report.getattr_init_rc==0 && default_report.getattr_rc==0);
    CHECK(default_report.getstacksize_rc==0 && default_report.getstack_rc==0 && default_report.getguardsize_rc==0);
    CHECK(default_report.query_destroy_rc==0 && default_report.measure_rc==0 && default_report.join_rc==0);
    printf("HOST NULL attributes observed stack=%llu guard=%llu; not PS4 evidence\n",
        (unsigned long long)default_report.reported_bytes,(unsigned long long)default_report.guard_bytes);
    x4_probe_stack_test_fail(3,EAGAIN);
    CHECK(x4_probe_default_stack_check(&default_report,&retained)==-EAGAIN); CHECK(!retained);
    CHECK(default_creates==1 && default_report.create_rc==EAGAIN);
    CHECK(!default_report.started && default_report.join_rc==X4_PROBE_STACK_NOT_RUN);
    x4_probe_stack_test_fail(4,EINVAL);
    CHECK(x4_probe_default_stack_check(&default_report,&retained)==-EINVAL); CHECK(!retained);
    CHECK(default_creates==2 && default_report.getattr_rc==EINVAL && default_report.join_rc==0);
    CHECK(default_report.started && default_report.finished && !default_report.pattern_ready);
    x4_probe_stack_test_fail(0,0); fail_join=EAGAIN;
    CHECK(x4_probe_default_stack_check(&default_report,&retained)==-EAGAIN); CHECK(retained);
    CHECK(default_creates==3 && default_report.measure_rc==X4_PROBE_STACK_RETAINED);
    CHECK(default_report.reported_bytes==0 && default_report.join_rc==EAGAIN);
    CHECK(x4_probe_default_stack_check(&default_report,&retained)==-EAGAIN); CHECK(retained);
    CHECK(default_creates==3); /* No new thread while its predecessor is retained. */
    fail_join=0;
    CHECK(!x4_probe_default_stack_check(&default_report,&retained)); CHECK(!retained);
    CHECK(default_creates==3 && default_report.reported_bytes>0 && default_report.join_rc==0);
    CHECK(default_report.finished && !default_report.pattern_ready);
    CHECK(!x4_probe_default_stack_check(&default_report,&retained)); CHECK(!retained && default_creates==4);
    /* Explicit stack path retains its original policy after all INFO cases. */
    CHECK(!x4_probe_thread_create(&id,t,work,&result,"x4-post-info")); join_thread(id,t);
    CHECK(t->report.configured_bytes==262144 && t->report.reported_bytes==262144 && calls==3);
    free(t);
    printf("PASS explicit stack host: %u checks; Linux pthread adapter; NOT native PS4 evidence\n",checks);return 0;
}
