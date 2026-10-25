/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_cpu.h"
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
#include <orbis/libkernel.h>
/* The installed header has an untyped void placeholder for this import.
 * Keep the SDK symbol/NID, with the two-argument x86_64 ABI independently
 * corroborated in research-selector/shad-pthread.cpp (pinned SHA in report).
 * This is an imported native function, not a POSIX or SDK stub. */
extern int32_t x4_native_pthread_setaffinity(OrbisPthread, uint64_t)
    __asm__("scePthreadSetaffinity");
#endif

X4StreamCpuConfig x4_stream_cpu_config(unsigned variant)
{
    if(variant>X4_CPU_3)variant=X4_CPU_0;
    return (X4StreamCpuConfig){variant,variant==X4_CPU_0?700:variant==X4_CPU_3?256:480,
        variant>=X4_CPU_2?0x0f:0x3f};
}
static void snapshot_init(X4StreamCpuSnapshot *s)
{*s=(X4StreamCpuSnapshot){.priority_result=X4_CPU_NOT_RUN,.attr_init_result=X4_CPU_NOT_RUN,
    .attr_get_result=X4_CPU_NOT_RUN,.affinity_result=X4_CPU_NOT_RUN,.attr_destroy_result=X4_CPU_NOT_RUN};}
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
static int snapshot_current(OrbisPthread self,X4StreamCpuSnapshot *s)
{
    snapshot_init(s);s->attempted=true;
    int32_t priority=0;s->priority_result=scePthreadGetprio(self,&priority);s->priority=priority;
    OrbisPthreadAttr attr;
    s->attr_init_result=scePthreadAttrInit(&attr);
    if(!s->attr_init_result){
        s->attr_get_result=scePthreadAttrGet(self,&attr);
        if(!s->attr_get_result)s->affinity_result=scePthreadAttrGetaffinity(&attr,&s->affinity);
        s->attr_destroy_result=scePthreadAttrDestroy(&attr);
    }
    int result=s->priority_result?s->priority_result:s->attr_init_result?s->attr_init_result:
        s->attr_get_result?s->attr_get_result:s->affinity_result?s->affinity_result:s->attr_destroy_result;
    s->valid=result==0;return result;
}
#endif
const char *x4_stream_cpu_outcome_name(X4StreamCpuOutcome outcome)
{
    static const char *names[]={"unchanged","applied","skipped_capture","restored","partial"};
    return (unsigned)outcome<sizeof(names)/sizeof(*names)?names[outcome]:"invalid";
}
int x4_stream_cpu_apply_current_thread(unsigned variant,X4StreamCpuRole role,
    unsigned index,X4StreamCpuReport *report)
{
    if(!report)return X4_CPU_INVALID;
    *report=(X4StreamCpuReport){.variant=variant,.role=(unsigned)role,.index=index,
        .priority_result=X4_CPU_NOT_RUN,.affinity_result=X4_CPU_NOT_RUN,
        .restore_priority_result=X4_CPU_NOT_RUN,.restore_affinity_result=X4_CPU_NOT_RUN};
    snapshot_init(&report->before);snapshot_init(&report->after);
    if(variant>X4_CPU_3||(unsigned)role>X4_CPU_ROLE_ASSEMBLY||
        (role==X4_CPU_ROLE_CONVERTER&&index>1))return report->result=X4_CPU_INVALID;
    if(variant<X4_CPU_2)return 0;
    report->requested_priority=role==X4_CPU_ROLE_DECODE?0:role==X4_CPU_ROLE_ASSEMBLY?500:700;
    report->requested_affinity=role==X4_CPU_ROLE_DECODE?0x0f:role==X4_CPU_ROLE_ASSEMBLY?0x20:((uint64_t)1<<(4+index));
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    OrbisPthread self=scePthreadSelf();
    int captured=snapshot_current(self,&report->before);
    if(captured){report->partial_applied=true;report->outcome=X4_CPU_SKIPPED_CAPTURE;
        report->result=captured;goto logged;}
    if(role!=X4_CPU_ROLE_DECODE){
        report->priority_attempted=true;
        report->priority_result=scePthreadSetprio(self,report->requested_priority);
    }
    if(!report->priority_attempted||!report->priority_result){
        report->affinity_attempted=true;
        report->affinity_result=x4_native_pthread_setaffinity(self,report->requested_affinity);
    }
    report->result=report->priority_attempted&&report->priority_result?report->priority_result:report->affinity_result;
    if(report->result){
        /* A setter error does not prove it left the previous value intact.
         * Restore every attempted field, using its real captured value. */
        if(report->priority_attempted){report->restore_priority_attempted=true;
            report->restore_priority_result=scePthreadSetprio(self,report->before.priority);}
        if(report->affinity_attempted){report->restore_affinity_attempted=true;
            report->restore_affinity_result=x4_native_pthread_setaffinity(self,report->before.affinity);}
    }
    int observed=snapshot_current(self,&report->after);
    if(!report->result&&observed)report->result=observed;
    if(!report->result){
        bool priority_ok=role==X4_CPU_ROLE_DECODE||report->after.priority==report->requested_priority;
        if(report->after.valid&&priority_ok&&report->after.affinity==report->requested_affinity)
            report->outcome=X4_CPU_APPLIED;
        else {report->outcome=X4_CPU_PARTIAL;report->result=X4_CPU_OBSERVATION;}
    }else if(report->after.valid&&report->after.priority==report->before.priority&&
        report->after.affinity==report->before.affinity)report->outcome=X4_CPU_RESTORED;
    else report->outcome=X4_CPU_PARTIAL;
    report->partial_applied=report->outcome!=X4_CPU_APPLIED;
logged:
    printf("XCloud4: stream CPU-%u role=%u index=%u priority=%d rc=%d affinity=0x%llx rc=%d outcome=%s partial=%u result=%d (nonfatal)\n",
        variant,(unsigned)role,index,report->requested_priority,report->priority_result,
        (unsigned long long)report->requested_affinity,report->affinity_result,
        x4_stream_cpu_outcome_name(report->outcome),report->partial_applied,report->result);
    printf("XCloud4: stream CPU capture before=%u prio=%d/%d attr=%d/%d/%d/%d affinity=0x%llx after=%u prio=%d/%d attr=%d/%d/%d/%d affinity=0x%llx restore_prio=%u/%d restore_affinity=%u/%d\n",
        report->before.valid,report->before.priority,report->before.priority_result,
        report->before.attr_init_result,report->before.attr_get_result,report->before.affinity_result,report->before.attr_destroy_result,
        (unsigned long long)report->before.affinity,report->after.valid,report->after.priority,report->after.priority_result,
        report->after.attr_init_result,report->after.attr_get_result,report->after.affinity_result,report->after.attr_destroy_result,
        (unsigned long long)report->after.affinity,report->restore_priority_attempted,report->restore_priority_result,
        report->restore_affinity_attempted,report->restore_affinity_result);
    return report->result;
#else
    return report->result=X4_CPU_INVALID;
#endif
}
static void write_snapshot(FILE *f,const X4StreamCpuSnapshot *s)
{
    fprintf(f,"{\"attempted\":%s,\"valid\":%s,\"priority\":%d,\"affinity\":%llu,\"priority_result\":%d,\"attr_init_result\":%d,\"attr_get_result\":%d,\"affinity_result\":%d,\"attr_destroy_result\":%d}",
        s->attempted?"true":"false",s->valid?"true":"false",s->priority,(unsigned long long)s->affinity,
        s->priority_result,s->attr_init_result,s->attr_get_result,s->affinity_result,s->attr_destroy_result);
}
void x4_stream_cpu_write(FILE *f,const X4StreamCpuReport *r)
{
    if(!f||!r)return;
    fprintf(f,"{\"variant\":%u,\"role\":%u,\"index\":%u,\"requested_priority\":%d,\"requested_affinity\":%llu,\"priority_attempted\":%s,\"priority_result\":%d,\"affinity_attempted\":%s,\"affinity_result\":%d,\"result\":%d,\"nonfatal\":true,\"outcome\":\"%s\",\"partial_applied\":%s,\"restore_priority_attempted\":%s,\"restore_priority_result\":%d,\"restore_affinity_attempted\":%s,\"restore_affinity_result\":%d,\"before\":",
        r->variant,r->role,r->index,r->requested_priority,(unsigned long long)r->requested_affinity,
        r->priority_attempted?"true":"false",r->priority_result,r->affinity_attempted?"true":"false",r->affinity_result,r->result,
        x4_stream_cpu_outcome_name(r->outcome),r->partial_applied?"true":"false",
        r->restore_priority_attempted?"true":"false",r->restore_priority_result,
        r->restore_affinity_attempted?"true":"false",r->restore_affinity_result);
    write_snapshot(f,&r->before);fputs(",\"after\":",f);write_snapshot(f,&r->after);fputc('}',f);
}
