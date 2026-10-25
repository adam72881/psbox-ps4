/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum { X4_PROBE_MEMORY_TYPES = 3, X4_PROBE_MEMORY_REPETITIONS = 5,
    X4_PROBE_MEMORY_BYTES = 4 * 1024 * 1024,
    X4_PROBE_MEMORY_ALIGNMENT = 16 * 1024,
    X4_PROBE_MEMORY_NOT_RUN = -2147483647,
    X4_PROBE_MEMORY_INVALID = -7401, X4_PROBE_MEMORY_CLOCK = -7402,
    X4_PROBE_MEMORY_CANCELLED = -7403, X4_PROBE_MEMORY_RETAINED = -7404,
    X4_PROBE_MEMORY_CONTENT = -7405 };
enum { X4_PROBE_MEMORY_READ, X4_PROBE_MEMORY_WRITE, X4_PROBE_MEMORY_COPY,
    X4_PROBE_MEMORY_OPERATIONS };

/* These indexes are not native memory type values. */
enum { X4_PROBE_MEMORY_ONION, X4_PROBE_MEMORY_WC_GARLIC,
    X4_PROBE_MEMORY_WB_GARLIC };
typedef struct {
    int native_type;
    int allocate_rc, map_rc, unmap_rc, release_rc, result;
    uint64_t setup_us, elapsed_us[X4_PROBE_MEMORY_OPERATIONS][X4_PROBE_MEMORY_REPETITIONS];
    unsigned completed[X4_PROBE_MEMORY_OPERATIONS];
    bool allocated, mapped, content_verified;
} X4ProbeMemoryRow;
typedef struct {
    X4ProbeMemoryRow rows[X4_PROBE_MEMORY_TYPES];
    unsigned active_type, active_operation, active_repetition, finished_types;
    uint64_t started_us, ended_us, checksum_sink;
    size_t bytes_per_block;
    int result;
    bool cancelled, cleanup_safe;
} X4ProbeMemoryResult;

/* A single owner runs this synchronously while the decoder is absent. A
 * maximum of one 4 MiB direct block and one 4 MiB ordinary heap buffer is live.
 * Success is zero for all backend operations. Failed unmap/release keeps the
 * direct allocation marked as retained; no unsafe retry or fallback occurs.
 * Host substitutions model call/results; they do not model PS4 bandwidth. */
typedef struct {
    void *context;
    int types[X4_PROBE_MEMORY_TYPES];
    int (*allocate)(void *, size_t, size_t, int, int64_t *);
    int (*map)(void *, int64_t, size_t, size_t, void **);
    int (*unmap)(void *, void *, size_t);
    int (*release)(void *, int64_t, size_t);
    void *(*heap_allocate)(void *, size_t);
    void (*heap_free)(void *, void *);
    uint64_t (*clock_us)(void *);
} X4ProbeMemoryBackend;

/* Called outside timed sections, between operations/repetitions and after
 * cleanup. Nonzero requests cancellation; it never skips required cleanup. */
typedef int (*X4ProbeMemoryProgress)(const X4ProbeMemoryResult *, void *);

const char *x4_probe_memory_type_name(unsigned index);
/* Decimal MB/s: bytes / elapsed microseconds. Zero means no valid sample. */
double x4_probe_memory_mb_s(uint64_t elapsed_us);
int x4_probe_memory_run(X4ProbeMemoryResult *, const X4ProbeMemoryBackend *,
    X4ProbeMemoryProgress, void *);
/* Existing validated /data exclusive-report policy; no arbitrary URL/path. */
int x4_probe_memory_export_json(const char *, const X4ProbeMemoryResult *);
