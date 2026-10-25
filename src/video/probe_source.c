/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "probe_source.h"
#if X4_VIDEO_PROBE
#include "probe_files.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

int x4_probe_schedule(uint64_t origin, uint64_t pts, uint64_t now,
    X4ProbeMode mode, uint64_t *wait, uint64_t *late)
{
    if (!wait || !late || (mode != X4_PROBE_PACED && mode != X4_PROBE_MAX_SPEED))
        return X4_PROBE_ARGUMENT;
    *wait = *late = 0;
    if (now < origin || pts > UINT64_MAX - origin) return X4_PROBE_CLOCK;
    uint64_t deadline = origin + pts;
    if (now > deadline) *late = now - deadline;
    if (mode == X4_PROBE_PACED && now < deadline) {
        *wait = deadline - now;
        return X4_PROBE_WAIT;
    }
    return X4_PROBE_OK;
}

#if X4_VIDEO_PROBE
typedef struct {
    FILE *clip, *index;
    const atomic_bool *cancel;
    bool cancelled, started, cached;
    X4ProbeMode mode;
    uint64_t ordinal, origin_us, last_now_us, previous_pts;
    X4ProbeAU next;
} ProbeState;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static bool cancel_requested(X4ProbeSource *source, ProbeState *state)
{
    if (!state->cancelled && state->cancel && atomic_load_explicit(state->cancel, memory_order_relaxed)) {
        state->cancelled = true; ++source->stats.cancel_observations;
    }
    return state->cancelled;
}
static int fail(X4ProbeSource *source, int result) { ++source->stats.errors; return result; }
static int file_size(FILE *file, uint64_t *size)
{
    if (fseeko(file, 0, SEEK_END)) return X4_PROBE_IO;
    off_t end = ftello(file);
    if (end < 0 || fseeko(file, 0, SEEK_SET)) return X4_PROBE_IO;
    *size = (uint64_t)end;
    return X4_PROBE_OK;
}
static bool local_path(const char *path)
{
    if (!path) return false;
    size_t length = strnlen(path, X4_PROBE_PATH_MAX + 1);
    return length && length <= X4_PROBE_PATH_MAX && !strstr(path, "://");
}
static int read_record(FILE *index, X4ProbeAU *au)
{
    uint8_t record[X4_PROBE_INDEX_RECORD];
    if (fread(record, 1, sizeof(record), index) != sizeof(record)) return X4_PROBE_IO;
    au->offset = le64(record); au->bytes = le64(record + 8); au->pts_us = le64(record + 16);
    return X4_PROBE_OK;
}

int x4_probe_source_close(X4ProbeSource *source)
{
    if (!source) return X4_PROBE_ARGUMENT;
    ProbeState *state = source->state;
    if (!state) return X4_PROBE_OK;
    int result = X4_PROBE_OK;
    if (state->clip && fclose(state->clip)) result = X4_PROBE_IO;
    if (state->index && fclose(state->index)) result = X4_PROBE_IO;
    free(state); source->state = NULL;
    if (result) ++source->stats.errors;
    return result;
}

static int source_open(X4ProbeSource *source, const char *clip_path,
    const char *index_path, const uint8_t expected[32], const uint8_t expected_index[32], X4ProbeHashFile hash,
    void *hash_context, const atomic_bool *cancel)
{
    if (!source || !local_path(clip_path) || !local_path(index_path) || !expected || !hash)
        return X4_PROBE_ARGUMENT;
    if (source->state) return X4_PROBE_STATE;
    memset(&source->info, 0, sizeof(source->info)); memset(&source->stats, 0, sizeof(source->stats));
    ProbeState *state = calloc(1, sizeof(*state));
    if (!state) return fail(source, X4_PROBE_MEMORY);
    source->state = state; state->cancel = cancel;
    int result = X4_PROBE_IO;
    if (cancel_requested(source, state)) { result = X4_PROBE_CANCELLED; goto reject; }
    #if X4_VIDEO_PROBE
    state->clip = x4_probe_files_sample_open(clip_path, NULL);
    state->index = x4_probe_files_sample_open(index_path, NULL);
#else
    state->clip = fopen(clip_path, "rb"); state->index = fopen(index_path, "rb");
#endif
    if (!state->clip || !state->index) goto reject;
    /* Read chunks explicitly: no stale stdio read-ahead may turn a truncated
     * clip into an apparently successful AU after identity verification. */
    if (setvbuf(state->clip, NULL, _IONBF, 0)) goto reject;
    uint64_t clip_bytes, index_bytes;
    if (file_size(state->clip, &clip_bytes) || file_size(state->index, &index_bytes)) goto reject;
    if (!clip_bytes || clip_bytes > X4_PROBE_FILE_MAX || index_bytes < X4_PROBE_INDEX_HEADER ||
        index_bytes > X4_PROBE_INDEX_HEADER + (uint64_t)X4_PROBE_MAX_RECORDS * X4_PROBE_INDEX_RECORD) {
        result = X4_PROBE_SIZE; goto reject;
    }
    uint8_t actual_index[32] = {0};
    if (expected_index) {
        result = hash(state->index, index_bytes, actual_index, cancel, hash_context);
        if (result == X4_PROBE_CANCELLED) x4_probe_source_cancel(source);
        if (result) goto reject;
        if (cancel_requested(source, state)) { result = X4_PROBE_CANCELLED; goto reject; }
        if (memcmp(actual_index, expected_index, sizeof(actual_index))) { result = X4_PROBE_HASH; goto reject; }
        if (fseeko(state->index, 0, SEEK_SET)) { result = X4_PROBE_IO; goto reject; }
        source->stats.index_hash_verified = true;
    }
    result = X4_PROBE_IO;
    uint8_t header[X4_PROBE_INDEX_HEADER];
    if (fread(header, 1, sizeof(header), state->index) != sizeof(header)) goto reject;
    if (memcmp(header, "X4AU1\0\0\0", 8) || le32(header + 8) != 1 || le32(header + 12) != sizeof(header)) {
        result = X4_PROBE_INDEX; goto reject;
    }
    X4ProbeSourceInfo info = {.file_bytes=le64(header+16), .records=le64(header+24),
        .duration_us=le64(header+32), .visible_width=le32(header+40), .visible_height=le32(header+44),
        .coded_width=le32(header+48), .coded_height=le32(header+52),
        .fps_num=le32(header+56), .fps_den=le32(header+60)};
    memcpy(info.source_sha256, header + 64, sizeof(info.source_sha256));
    memcpy(info.index_sha256, actual_index, sizeof(info.index_sha256));
    if (info.file_bytes != clip_bytes || !info.records || info.records > X4_PROBE_MAX_RECORDS ||
        index_bytes != X4_PROBE_INDEX_HEADER + info.records * X4_PROBE_INDEX_RECORD ||
        !info.duration_us || info.duration_us > X4_PROBE_DURATION_MAX_US ||
        !info.visible_width || info.visible_width > 1920 || !info.visible_height || info.visible_height > 1080 ||
        info.coded_width < info.visible_width || info.coded_width > 1920 ||
        info.coded_height < info.visible_height || info.coded_height > 1088 ||
        ((info.visible_width | info.visible_height | info.coded_width | info.coded_height) & 1) ||
        !info.fps_num || !info.fps_den || info.fps_den > 1000000 || (uint64_t)info.fps_num > 120u * (uint64_t)info.fps_den) {
        result = X4_PROBE_INDEX; goto reject;
    }
    if (memcmp(info.source_sha256, expected, sizeof(info.source_sha256))) {
        result = X4_PROBE_HASH; goto reject;
    }
    uint64_t offset = 0, previous_pts = 0;
    for (uint64_t i = 0; i < info.records; ++i) {
        if (cancel_requested(source, state)) { result = X4_PROBE_CANCELLED; goto reject; }
        X4ProbeAU au = {0};
        if (read_record(state->index, &au)) goto reject;
        if (au.bytes < 5 || au.bytes > X4_PROBE_AU_MAX) { result = X4_PROBE_SIZE; goto reject; }
        if (au.offset != offset || au.offset > clip_bytes || au.bytes > clip_bytes - au.offset ||
            au.pts_us >= info.duration_us || (!i && au.pts_us) || (i && au.pts_us <= previous_pts)) {
            result = X4_PROBE_INDEX; goto reject;
        }
        offset += au.bytes; previous_pts = au.pts_us; ++source->stats.validated_records;
    }
    if (offset != clip_bytes) { result = X4_PROBE_INDEX; goto reject; }
    uint8_t actual[32] = {0};
    result = hash(state->clip, clip_bytes, actual, cancel, hash_context);
    if (result == X4_PROBE_CANCELLED) x4_probe_source_cancel(source);
    if (result) goto reject;
    if (cancel_requested(source, state)) { result = X4_PROBE_CANCELLED; goto reject; }
    if (memcmp(actual, expected, sizeof(actual))) { result = X4_PROBE_HASH; goto reject; }
    if (fseeko(state->clip, 0, SEEK_SET) || fseeko(state->index, X4_PROBE_INDEX_HEADER, SEEK_SET)) {
        result = X4_PROBE_IO; goto reject;
    }
    source->info = info; source->stats.hash_verified = true;
    return X4_PROBE_OK;
reject:
    x4_probe_source_close(source);
    if (result != X4_PROBE_CANCELLED) ++source->stats.errors;
    return result;
}
int x4_probe_source_open_verified(X4ProbeSource *source, const char *clip_path,
    const char *index_path, const uint8_t expected[32], X4ProbeHashFile hash,
    void *context, const atomic_bool *cancel)
{
    return source_open(source, clip_path, index_path, expected, NULL, hash, context, cancel);
}
int x4_probe_source_open_fully_verified(X4ProbeSource *source, const char *clip_path,
    const char *index_path, const uint8_t expected[32], const uint8_t expected_index[32],
    X4ProbeHashFile hash, void *context, const atomic_bool *cancel)
{
    if (!expected_index) return X4_PROBE_ARGUMENT;
    return source_open(source, clip_path, index_path, expected, expected_index, hash, context, cancel);
}

int x4_probe_source_begin(X4ProbeSource *source, uint64_t origin, X4ProbeMode mode)
{
    if (!source || !source->state || (mode != X4_PROBE_PACED && mode != X4_PROBE_MAX_SPEED))
        return X4_PROBE_ARGUMENT;
    ProbeState *state = source->state;
    if (cancel_requested(source, state)) return X4_PROBE_CANCELLED;
    if (state->started || state->ordinal) return X4_PROBE_STATE;
    state->started = true; state->mode = mode; state->origin_us = state->last_now_us = origin;
    return X4_PROBE_OK;
}

int x4_probe_source_next(X4ProbeSource *source, uint64_t now, uint8_t *destination,
    size_t destination_capacity, X4ProbeAU *out)
{
    if (!source || !source->state || !destination || !out) return X4_PROBE_ARGUMENT;
    memset(out, 0, sizeof(*out));
    ProbeState *state = source->state;
    if (cancel_requested(source, state)) return X4_PROBE_CANCELLED;
    if (!state->started) return X4_PROBE_STATE;
    if (now < state->last_now_us) return fail(source, X4_PROBE_CLOCK);
    state->last_now_us = now;
    if (state->ordinal == source->info.records) return X4_PROBE_EOF;
    if (!state->cached) {
        memset(&state->next, 0, sizeof(state->next));
        if (read_record(state->index, &state->next)) return fail(source, X4_PROBE_IO);
        if (state->next.bytes < 5 || state->next.bytes > X4_PROBE_AU_MAX ||
            state->next.offset > source->info.file_bytes ||
            state->next.bytes > source->info.file_bytes - state->next.offset ||
            state->next.pts_us >= source->info.duration_us ||
            (!state->ordinal && state->next.pts_us) ||
            (state->ordinal && state->next.pts_us <= state->previous_pts) ||
            (state->ordinal + 1 == source->info.records &&
             state->next.offset + state->next.bytes != source->info.file_bytes))
            return fail(source, X4_PROBE_INDEX);
        state->next.ordinal = state->ordinal; state->cached = true;
    }
    *out = state->next;
    int result = x4_probe_schedule(state->origin_us, out->pts_us, now, state->mode, &out->wait_us, &out->late_us);
    if (result == X4_PROBE_WAIT) { ++source->stats.wait_observations; return result; }
    if (result) return fail(source, result);
    if (out->bytes > destination_capacity || out->bytes > X4_PROBE_AU_MAX)
        return fail(source, X4_PROBE_SIZE);
    /* File offsets were validated once; insist on the expected position to
     * avoid silently consuming a different range after an I/O failure. */
    off_t current = ftello(state->clip);
    if (current < 0 || (uint64_t)current != out->offset) return fail(source, X4_PROBE_IO);
    size_t remaining = (size_t)out->bytes, copied = 0;
    while (remaining) {
        if (cancel_requested(source, state)) return X4_PROBE_CANCELLED;
        size_t bytes = remaining > X4_PROBE_READ_CHUNK ? X4_PROBE_READ_CHUNK : remaining;
        size_t received = fread(destination + copied, 1, bytes, state->clip);
        ++source->stats.read_calls; source->stats.bytes_read += received;
        if (received != bytes) return fail(source, X4_PROBE_IO);
        copied += received; remaining -= received;
    }
    if (cancel_requested(source, state)) return X4_PROBE_CANCELLED;
    bool annexb = (copied >= 4 && !destination[0] && !destination[1] && destination[2] == 1) ||
        (copied >= 5 && !destination[0] && !destination[1] && !destination[2] && destination[3] == 1);
    if (!annexb) return fail(source, X4_PROBE_ANNEXB);
    state->previous_pts = out->pts_us;
    ++state->ordinal; state->cached = false; ++source->stats.aus_read;
    if (out->late_us) {
        ++source->stats.late_aus;
        if (out->late_us > source->stats.max_late_us) source->stats.max_late_us = out->late_us;
    }
    return X4_PROBE_OK;
}

void x4_probe_source_cancel(X4ProbeSource *source)
{
    ProbeState *state = source ? source->state : NULL;
    if (state && !state->cancelled) { state->cancelled = true; ++source->stats.cancel_observations; }
}
#else
int x4_probe_source_open_verified(X4ProbeSource *source, const char *clip,
    const char *index, const uint8_t expected[32], X4ProbeHashFile hash,
    void *context, const atomic_bool *cancel)
{
    (void)source; (void)clip; (void)index; (void)expected; (void)hash; (void)context; (void)cancel;
    return X4_PROBE_DISABLED;
}
int x4_probe_source_open_fully_verified(X4ProbeSource *source, const char *clip,
    const char *index, const uint8_t expected[32], const uint8_t expected_index[32],
    X4ProbeHashFile hash, void *context, const atomic_bool *cancel)
{
    (void)source; (void)clip; (void)index; (void)expected; (void)expected_index;
    (void)hash; (void)context; (void)cancel; return X4_PROBE_DISABLED;
}
int x4_probe_source_begin(X4ProbeSource *source, uint64_t origin, X4ProbeMode mode)
{ (void)source; (void)origin; (void)mode; return X4_PROBE_DISABLED; }
int x4_probe_source_next(X4ProbeSource *source, uint64_t now, uint8_t *dst, size_t cap, X4ProbeAU *out)
{ (void)source; (void)now; (void)dst; (void)cap; (void)out; return X4_PROBE_DISABLED; }
void x4_probe_source_cancel(X4ProbeSource *source) { (void)source; }
int x4_probe_source_close(X4ProbeSource *source) { return source ? X4_PROBE_OK : X4_PROBE_ARGUMENT; }
#endif
