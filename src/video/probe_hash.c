/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_hash.h"
#include <stdlib.h>
#if X4_VIDEO_PROBE
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#define X4_SHA_START mbedtls_sha256_starts
#define X4_SHA_UPDATE mbedtls_sha256_update
#define X4_SHA_FINISH mbedtls_sha256_finish
#else
#define X4_SHA_START mbedtls_sha256_starts_ret
#define X4_SHA_UPDATE mbedtls_sha256_update_ret
#define X4_SHA_FINISH mbedtls_sha256_finish_ret
#endif
int x4_probe_hash_context_init(X4ProbeHashContext *context)
{
    if (!context) return X4_PROBE_ARGUMENT;
    if (context->buffer || context->capacity) return X4_PROBE_STATE;
    context->buffer = malloc(X4_PROBE_READ_CHUNK);
    if (!context->buffer) return X4_PROBE_MEMORY;
    context->capacity = X4_PROBE_READ_CHUNK;
    return 0;
}
void x4_probe_hash_context_close(X4ProbeHashContext *context)
{
    if (!context) return;
    free(context->buffer); context->buffer = NULL; context->capacity = 0;
}
int x4_probe_hash_file(FILE *file, uint64_t bytes, uint8_t digest[32],
    const atomic_bool *cancel, void *opaque)
{
    if (!file || !digest || bytes > X4_PROBE_FILE_MAX) return X4_PROBE_ARGUMENT;
    X4ProbeHashContext *scratch = opaque;
    if (!scratch || !scratch->buffer || scratch->capacity != X4_PROBE_READ_CHUNK)
        return X4_PROBE_STATE;
    mbedtls_sha256_context context; mbedtls_sha256_init(&context);
    int result = X4_SHA_START(&context, 0);
    uint64_t read = 0;
    while (!result && read < bytes) {
        if (cancel && atomic_load_explicit(cancel, memory_order_acquire)) { result = X4_PROBE_CANCELLED; break; }
        size_t wanted = bytes - read < scratch->capacity ? (size_t)(bytes - read) : scratch->capacity;
        size_t got = fread(scratch->buffer, 1, wanted, file);
        if (got != wanted) { result = X4_PROBE_IO; break; }
        result = X4_SHA_UPDATE(&context, scratch->buffer, got); read += got;
    }
    if (!result && fgetc(file) != EOF) result = X4_PROBE_SIZE;
    if (!result && ferror(file)) result = X4_PROBE_IO;
    if (!result) result = X4_SHA_FINISH(&context, digest);
    mbedtls_sha256_free(&context);
    return result;
}
#else
int x4_probe_hash_context_init(X4ProbeHashContext *context)
{ (void)context; return X4_PROBE_DISABLED; }
void x4_probe_hash_context_close(X4ProbeHashContext *context) { (void)context; }
int x4_probe_hash_file(FILE *file, uint64_t bytes, uint8_t digest[32], const atomic_bool *cancel, void *unused)
{ (void)file; (void)bytes; (void)digest; (void)cancel; (void)unused; return X4_PROBE_DISABLED; }
#endif
