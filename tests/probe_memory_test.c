/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/probe_memory.h"
#include "../src/video/probe_files.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { F_NONE, F_ALLOC, F_MAP, F_UNMAP, F_RELEASE, F_HEAP, F_ALIGNMENT, F_CLOCK, F_CONTENT };
typedef struct {
    unsigned allocated, mapped, unmapped, released, heap_allocated, heap_freed;
    unsigned progress_calls, cancel_at, fault, clock_calls;
    uint64_t now;
    void *direct, *heap;
} Mock;
static int allocate(void *ctx,size_t bytes,size_t alignment,int type,int64_t *offset)
{
    Mock *m=ctx; assert(bytes==X4_PROBE_MEMORY_BYTES && alignment==X4_PROBE_MEMORY_ALIGNMENT);
    assert(type==0 || type==3 || type==10); assert(!m->direct); ++m->allocated;
    if(m->fault==F_ALLOC) return -801;
    m->direct=aligned_alloc(alignment,bytes); assert(m->direct); *offset=123; return 0;
}
static int map(void *ctx,int64_t offset,size_t bytes,size_t alignment,void **address)
{
    Mock *m=ctx; ++m->mapped; assert(offset==123 && bytes==X4_PROBE_MEMORY_BYTES && alignment==X4_PROBE_MEMORY_ALIGNMENT);
    if(m->fault==F_MAP) return -802;
    *address=(unsigned char *)m->direct+(m->fault==F_ALIGNMENT ? 1:0); return 0;
}
static int unmap(void *ctx,void *address,size_t bytes)
{
    Mock *m=ctx; ++m->unmapped; assert(bytes==X4_PROBE_MEMORY_BYTES);
    assert(address==m->direct || address==(unsigned char *)m->direct+1);
    return m->fault==F_UNMAP ? -803:0;
}
static int release(void *ctx,int64_t offset,size_t bytes)
{
    Mock *m=ctx; ++m->released; assert(offset==123 && bytes==X4_PROBE_MEMORY_BYTES);
    if(m->fault==F_RELEASE) return -804;
    free(m->direct); m->direct=NULL; return 0;
}
static void *heap_allocate(void *ctx,size_t bytes)
{
    Mock *m=ctx; ++m->heap_allocated; assert(bytes==X4_PROBE_MEMORY_BYTES);
    if(m->fault==F_HEAP) return NULL;
    m->heap=malloc(bytes); assert(m->heap); return m->heap;
}
static void heap_free(void *ctx,void *address)
{ Mock *m=ctx; ++m->heap_freed; assert(address==m->heap); free(address); m->heap=NULL; }
static uint64_t clock_us(void *ctx)
{
    Mock *m=ctx; ++m->clock_calls;
    if(m->fault==F_CONTENT && m->heap) ((unsigned char *)m->heap)[0]=0;
    if(m->fault!=F_CLOCK) m->now+=100;
    return m->now;
}
static X4ProbeMemoryBackend backend(Mock *m)
{
    /* Host model IDs only; native builds use verified installed SDK macros. */
    return (X4ProbeMemoryBackend){m,{0,3,10},allocate,map,unmap,release,heap_allocate,heap_free,clock_us};
}
static int progress(const X4ProbeMemoryResult *r,void *ctx)
{
    Mock *m=ctx; ++m->progress_calls;
    assert(r->active_type<X4_PROBE_MEMORY_TYPES && r->active_operation<X4_PROBE_MEMORY_OPERATIONS);
    return m->cancel_at && m->progress_calls==m->cancel_at;
}
static void run_failure(unsigned fault,int expected)
{
    Mock m={.fault=fault}; X4ProbeMemoryBackend b=backend(&m); X4ProbeMemoryResult r;
    assert(x4_probe_memory_run(&r,&b,progress,&m)==expected);
    assert(!r.finished_types && !r.cancelled);
    assert(m.allocated<=1 && m.heap_allocated==1); /* Never try a later type on failure. */
    if(fault==F_HEAP) { assert(!m.allocated && !m.heap_freed && r.cleanup_safe); return; }
    assert(m.heap_freed==1 && !m.heap);
    assert(r.rows[1].allocate_rc==X4_PROBE_MEMORY_NOT_RUN);
    if(fault==F_UNMAP || fault==F_RELEASE) {
        assert(!r.cleanup_safe && r.rows[0].allocated);
        if(fault==F_UNMAP) { assert(r.rows[0].mapped && !m.released); }
        /* Test-only cleanup of a synthetic allocation after verifying the
         * production backend intentionally retained it on unknown lifetime. */
        free(m.direct); m.direct=NULL;
    } else { assert(r.cleanup_safe && !m.direct); }
}
int main(int argc,char **argv)
{
    assert(argc==2); Mock m={0}; X4ProbeMemoryBackend b=backend(&m); X4ProbeMemoryResult r;
    assert(!x4_probe_memory_run(&r,&b,progress,&m));
    assert(r.finished_types==3 && r.cleanup_safe && !r.cancelled);
    assert(m.allocated==3 && m.mapped==3 && m.unmapped==3 && m.released==3);
    assert(m.heap_allocated==1 && m.heap_freed==1 && !m.direct && !m.heap);
    for(unsigned i=0;i<3;++i) {
        assert(!r.rows[i].result && r.rows[i].content_verified);
        for(unsigned op=0;op<3;++op) {
            assert(r.rows[i].completed[op]==5);
            for(unsigned n=0;n<5;++n) assert(r.rows[i].elapsed_us[op][n]==100);
        }
    }
    assert(x4_probe_memory_mb_s(100)==41943.04);
    assert(x4_probe_memory_mb_s(0)==0.0);
    assert(!strcmp(x4_probe_memory_type_name(0),"WB_ONION"));
    assert(!strcmp(x4_probe_memory_type_name(3),"INVALID"));
    assert(!x4_probe_files_test_root(argv[1]));
    char report[X4_PROBE_FILE_PATH_MAX];
    assert(!x4_probe_files_report_path(report,sizeof(report),"memory",1));
    assert(!x4_probe_memory_export_json(report,&r));
    assert(x4_probe_memory_export_json(report,&r)==-1 && errno==EEXIST);
    assert(x4_probe_memory_export_json("/tmp/arbitrary.json",&r)==-1);
    assert(x4_probe_memory_run(NULL,&b,NULL,NULL)==X4_PROBE_MEMORY_INVALID);
    X4ProbeMemoryBackend bad=b; bad.map=NULL;
    assert(x4_probe_memory_run(&r,&bad,NULL,NULL)==X4_PROBE_MEMORY_INVALID);
    bad=b; bad.types[1]=bad.types[0];
    assert(x4_probe_memory_run(&r,&bad,NULL,NULL)==X4_PROBE_MEMORY_INVALID);
    m=(Mock){.cancel_at=2}; b=backend(&m);
    assert(x4_probe_memory_run(&r,&b,progress,&m)==X4_PROBE_MEMORY_CANCELLED);
    assert(r.cancelled && r.cleanup_safe && m.allocated==1 && m.unmapped==1 && m.released==1);
    assert(!r.rows[0].completed[0] && m.heap_freed==1);
    run_failure(F_ALLOC,-801); run_failure(F_MAP,-802);
    run_failure(F_ALIGNMENT,X4_PROBE_MEMORY_INVALID);
    run_failure(F_CLOCK,X4_PROBE_MEMORY_CLOCK);
    run_failure(F_HEAP,-ENOMEM); run_failure(F_CONTENT,X4_PROBE_MEMORY_CONTENT);
    run_failure(F_UNMAP,-803); run_failure(F_RELEASE,-804);
    /* Cancelling in the final progress callback must not hide the earlier
     * native failure. UI stops on result even if cancelled is also true. */
    for(unsigned fault=F_ALLOC;fault<=F_MAP;++fault) {
        m=(Mock){.fault=fault,.cancel_at=2}; b=backend(&m);
        int expected=fault==F_ALLOC ? -801 : -802;
        assert(x4_probe_memory_run(&r,&b,progress,&m)==expected);
        assert(r.result==expected && r.rows[0].result==expected && r.cancelled);
        assert(r.cleanup_safe && m.heap_freed==1 && !m.direct && !m.heap);
    }
    puts("PASS probe memory: bounded allocation, 3 types/5 repetitions, cancellation, timing, content, native faults, teardown retention, exclusive JSON");
    return 0;
}
