/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_memory.h"
#if X4_VIDEO_PROBE || defined(X4_PROBE_MEMORY_TESTING)
#include "probe_files.h"
#include "../core/build_identity.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#else
#include <stdatomic.h>
#endif
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#endif

static const char *const names[X4_PROBE_MEMORY_TYPES] = {"WB_ONION", "WC_GARLIC", "WB_GARLIC"};
static const char *const operations[X4_PROBE_MEMORY_OPERATIONS] = {"sequential_read", "sequential_write", "memcpy_to_process_heap"};
static volatile uint64_t benchmark_sink;
/* A failed native teardown is terminal for this process; do not accumulate a
 * second direct block by pressing the menu item again. */
static bool native_retained;

const char *x4_probe_memory_type_name(unsigned index)
{ return index < X4_PROBE_MEMORY_TYPES ? names[index] : "INVALID"; }
double x4_probe_memory_mb_s(uint64_t elapsed_us)
{ return elapsed_us ? (double)X4_PROBE_MEMORY_BYTES / (double)elapsed_us : 0.0; }

static void memory_fence(void)
{
#if defined(__x86_64__) || defined(__i386__)
    /* WC stores must drain before stopping the CPU timing. This does not
     * flush WB cache lines and does not establish GPU/decoder coherence. */
    _mm_mfence();
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

#ifdef X4_OPENORBIS
static int native_allocate(void *context, size_t size, size_t alignment, int type, int64_t *offset)
{
    (void)context; off_t native_offset = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, alignment, type, &native_offset);
    if (!rc) *offset = (int64_t)native_offset;
    return rc;
}
static int native_map(void *context, int64_t offset, size_t size, size_t alignment, void **out)
{
    (void)context;
    /* CPU read/write only. The decoder experiment separately retains its
     * existing CPU/GPU protections; no decoder is present in this benchmark. */
    return sceKernelMapDirectMemory(out, size, 0x03, 0, (off_t)offset, alignment);
}
static int native_unmap(void *context, void *address, size_t size)
{ (void)context; return sceKernelMunmap(address, size); }
static int native_release(void *context, int64_t offset, size_t size)
{ (void)context; return sceKernelReleaseDirectMemory((off_t)offset, size); }
static void *native_heap_allocate(void *context, size_t size)
{ (void)context; return malloc(size); }
static void native_heap_free(void *context, void *address)
{ (void)context; free(address); }
static uint64_t native_clock(void *context)
{ (void)context; return sceKernelGetProcessTime(); }
static const X4ProbeMemoryBackend native_backend = {
    NULL,
    /* Resolve SDK memory types at compilation; export their numeric values. */
    {ORBIS_KERNEL_WB_ONION, ORBIS_KERNEL_WC_GARLIC, ORBIS_KERNEL_WB_GARLIC},
    native_allocate, native_map, native_unmap, native_release,
    native_heap_allocate, native_heap_free, native_clock
};
#endif

static void first_failure(X4ProbeMemoryResult *r, X4ProbeMemoryRow *row, int rc)
{
    if (rc && !r->result) r->result = rc;
    if (rc && !row->result) row->result = rc;
}

static bool progress_or_cancel(X4ProbeMemoryResult *r, X4ProbeMemoryRow *row,
    X4ProbeMemoryProgress progress, void *context)
{
    if (progress && progress(r, context)) {
        r->cancelled = true;
        first_failure(r, row, X4_PROBE_MEMORY_CANCELLED);
    }
    return !r->result;
}

/* Volatile loads/stores prevent dead-code elimination. Four accumulators
 * reduce the loop dependency without introducing MOVNTDQA or a GPU path.
 * This is a CPU synthetic workload, not the four-reader decoder copy. */
static uint64_t sequential_read(const void *address)
{
    const volatile uint64_t *p = address;
    uint64_t a = 0, b = 0, c = 0, d = 0;
    for (size_t i = 0; i < X4_PROBE_MEMORY_BYTES / sizeof(*p); i += 4) {
        a += p[i]; b += p[i+1]; c += p[i+2]; d += p[i+3];
    }
    return a + b + c + d;
}
static void sequential_write(void *address)
{
    volatile uint64_t *p = address;
    for (size_t i = 0; i < X4_PROBE_MEMORY_BYTES / sizeof(*p); i += 4) {
        p[i] = UINT64_C(0x5a5a5a5a5a5a5a5a); p[i+1] = UINT64_C(0x5a5a5a5a5a5a5a5a);
        p[i+2] = UINT64_C(0x5a5a5a5a5a5a5a5a); p[i+3] = UINT64_C(0x5a5a5a5a5a5a5a5a);
    }
}
static int valid_backend(const X4ProbeMemoryBackend *b)
{
    return b && b->allocate && b->map && b->unmap && b->release &&
        b->heap_allocate && b->heap_free && b->clock_us &&
        b->types[0] != b->types[1] && b->types[1] != b->types[2] && b->types[0] != b->types[2];
}

int x4_probe_memory_run(X4ProbeMemoryResult *r, const X4ProbeMemoryBackend *provided,
    X4ProbeMemoryProgress progress, void *context)
{
    if (!r) return X4_PROBE_MEMORY_INVALID;
    memset(r, 0, sizeof(*r));
    r->bytes_per_block = X4_PROBE_MEMORY_BYTES; r->cleanup_safe = true;
    const X4ProbeMemoryBackend *b = provided;
#ifdef X4_OPENORBIS
    if (!b) b = &native_backend;
#endif
    for (unsigned i = 0; i < X4_PROBE_MEMORY_TYPES; ++i) {
        X4ProbeMemoryRow *row = &r->rows[i];
        row->allocate_rc = row->map_rc = row->unmap_rc = row->release_rc = X4_PROBE_MEMORY_NOT_RUN;
        row->result = X4_PROBE_MEMORY_NOT_RUN;
        row->native_type = b ? b->types[i] : -1;
    }
    if (!valid_backend(b)) return r->result = X4_PROBE_MEMORY_INVALID;
    if (!provided && native_retained) { r->cleanup_safe = false; return r->result = X4_PROBE_MEMORY_RETAINED; }
    r->started_us = b->clock_us(b->context);
    void *heap = b->heap_allocate(b->context, X4_PROBE_MEMORY_BYTES);
    if (!heap) { r->ended_us = b->clock_us(b->context); return r->result = -ENOMEM; }
    /* Heap pages are faulted before timed memcpy; this work is excluded. */
    memset(heap, 0, X4_PROBE_MEMORY_BYTES);
    for (unsigned i = 0; i < X4_PROBE_MEMORY_TYPES && !r->result; ++i) {
        X4ProbeMemoryRow *row = &r->rows[i];
        row->result = 0;
        r->active_type = i; r->active_operation = 0; r->active_repetition = 0;
        if (!progress_or_cancel(r, row, progress, context)) break;
        void *direct = NULL; int64_t offset = 0;
        uint64_t setup_start = b->clock_us(b->context);
        row->allocate_rc = b->allocate(b->context, X4_PROBE_MEMORY_BYTES, X4_PROBE_MEMORY_ALIGNMENT, row->native_type, &offset);
        first_failure(r, row, row->allocate_rc);
        if (!row->allocate_rc) {
            row->allocated = true;
            row->map_rc = b->map(b->context, offset, X4_PROBE_MEMORY_BYTES, X4_PROBE_MEMORY_ALIGNMENT, &direct);
            first_failure(r, row, row->map_rc);
            if (!row->map_rc) {
                row->mapped = true;
                if (!direct || ((uintptr_t)direct % X4_PROBE_MEMORY_ALIGNMENT))
                    first_failure(r, row, X4_PROBE_MEMORY_INVALID);
            }
        }
        if (!r->result) { sequential_write(direct); memory_fence(); }
        uint64_t setup_end = b->clock_us(b->context);
        if (setup_end < setup_start) first_failure(r, row, X4_PROBE_MEMORY_CLOCK);
        else row->setup_us = setup_end - setup_start;
        for (unsigned op = 0; op < X4_PROBE_MEMORY_OPERATIONS && !r->result; ++op) {
            r->active_operation = op;
            for (unsigned repetition = 0; repetition < X4_PROBE_MEMORY_REPETITIONS && !r->result; ++repetition) {
                r->active_repetition = repetition;
                if (!progress_or_cancel(r, row, progress, context)) break;
                memory_fence();
                uint64_t start = b->clock_us(b->context), sum = 0;
                if (op == X4_PROBE_MEMORY_READ) sum = sequential_read(direct);
                else if (op == X4_PROBE_MEMORY_WRITE) sequential_write(direct);
                else memcpy(heap, direct, X4_PROBE_MEMORY_BYTES);
                memory_fence();
                uint64_t end = b->clock_us(b->context);
                benchmark_sink ^= sum; r->checksum_sink ^= sum;
                if (end <= start) first_failure(r, row, X4_PROBE_MEMORY_CLOCK);
                else {
                    row->elapsed_us[op][repetition] = end - start;
                    ++row->completed[op];
                }
            }
        }
        if (!r->result) {
            /* Validation is outside all timings and independent of memcpy's
             * return value. Initial write and later writes use the same bytes. */
            const unsigned char *p = heap;
            row->content_verified = true;
            for (size_t j = 0; j < X4_PROBE_MEMORY_BYTES; ++j)
                if (p[j] != 0x5a) { row->content_verified = false; first_failure(r,row,X4_PROBE_MEMORY_CONTENT); break; }
        }
        if (row->mapped) {
            row->unmap_rc = b->unmap(b->context, direct, X4_PROBE_MEMORY_BYTES);
            first_failure(r, row, row->unmap_rc);
            if (!row->unmap_rc) row->mapped = false;
        }
        /* Never release backing memory still mapped after a failed unmap. */
        if (row->allocated && !row->mapped) {
            row->release_rc = b->release(b->context, offset, X4_PROBE_MEMORY_BYTES);
            first_failure(r, row, row->release_rc);
            if (!row->release_rc) row->allocated = false;
        }
        if (row->allocated || row->mapped) { r->cleanup_safe = false; if (!provided) native_retained = true; }
        if (!row->result) ++r->finished_types;
        (void)progress_or_cancel(r, row, progress, context);
    }
    b->heap_free(b->context, heap);
    r->ended_us = b->clock_us(b->context);
    if (r->ended_us < r->started_us && !r->result) r->result = X4_PROBE_MEMORY_CLOCK;
    return r->result;
}

int x4_probe_memory_export_json(const char *path, const X4ProbeMemoryResult *r)
{
    if (!r || !path) { errno = EINVAL; return -1; }
    FILE *file = x4_probe_files_report_create(path, NULL);
    if (!file) return -1;
    int failed = fprintf(file, "{\n\"schema\":\"X4PROBE_MEMORY1\",\n\"build_id\":\"%s\",\n"
        "\"result\":%d,\"cleanup_safe\":%s,\"cancelled\":%s,\"bytes_per_block\":%zu,"
        "\"repetitions\":%u,\"finished_types\":%u,\"started_us\":%llu,\"ended_us\":%llu,\n"
        "\"method\":\"single CPU, volatile 64-bit sequential read/write, ordinary memcpy to pre-touched heap; mfence included; no cache flush; repeated hot-cache samples; no decoder\",\n"
        "\"units\":\"microseconds and decimal MB/s\",\"rows\":[\n",
        X4_BUILD_ID, r->result, r->cleanup_safe ? "true":"false", r->cancelled ? "true":"false", r->bytes_per_block,
        X4_PROBE_MEMORY_REPETITIONS, r->finished_types,
        (unsigned long long)r->started_us, (unsigned long long)r->ended_us) < 0;
    for (unsigned i = 0; i < X4_PROBE_MEMORY_TYPES; ++i) {
        const X4ProbeMemoryRow *row = &r->rows[i];
        if (fprintf(file, "%s{\"name\":\"%s\",\"native_type\":%d,\"cpu_protection\":3,"
            "\"result\":%d,\"allocate_rc\":%d,\"map_rc\":%d,\"unmap_rc\":%d,\"release_rc\":%d,"
            "\"allocation_retained\":%s,\"mapping_retained\":%s,\"content_verified\":%s,\"setup_us\":%llu,\"operations\":[",
            i ? ",\n":"", names[i], row->native_type, row->result, row->allocate_rc, row->map_rc,
            row->unmap_rc, row->release_rc, row->allocated ? "true":"false", row->mapped ? "true":"false",
            row->content_verified ? "true":"false", (unsigned long long)row->setup_us) < 0) failed = 1;
        for (unsigned op = 0; op < X4_PROBE_MEMORY_OPERATIONS; ++op) {
            if (fprintf(file, "%s{\"operation\":\"%s\",\"completed\":%u,\"elapsed_us\":[",
                op ? ",":"", operations[op], row->completed[op]) < 0) failed = 1;
            for (unsigned j = 0; j < X4_PROBE_MEMORY_REPETITIONS; ++j)
                if (fprintf(file,"%s%llu",j ? ",":"",(unsigned long long)row->elapsed_us[op][j]) < 0) failed = 1;
            if (fprintf(file, "],\"MB_per_s\":[") < 0) failed = 1;
            for (unsigned j = 0; j < X4_PROBE_MEMORY_REPETITIONS; ++j)
                if (fprintf(file,"%s%.6f",j ? ",":"",x4_probe_memory_mb_s(row->elapsed_us[op][j])) < 0) failed = 1;
            if (fprintf(file,"]}") < 0) failed = 1;
        }
        if (fprintf(file,"]}") < 0) failed = 1;
    }
    if (fprintf(file,"\n]}\n") < 0) failed = 1;
    int saved = failed ? (errno ? errno : EIO) : 0;
    int finish = x4_probe_files_report_finish(file, NULL);
    if (saved) { errno = saved; return -1; }
    return finish;
}
#endif
