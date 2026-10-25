/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#define X4_PROBE_REPORT_ROOT "/data"
#define X4_PROBE_SAMPLE_ROOT "/data/xcloud4-probe"
#define X4_PROBE_FILE_PATH_MAX 320

typedef struct {
    int result;             /* 0 success, -1 failure; not a native result. */
    int error_number;       /* captured errno or explicit policy error; 0 success. */
    int native_result;      /* raw libkernel error, zero for POSIX operations. */
    const char *operation;  /* static description, no payload or credentials. */
    int64_t raw_return;     /* Immediate result of the named call. */
    int call_errno;         /* Immediate errno, including successful calls. */
    const char *call;
} X4ProbeFileStatus;

typedef struct {
    const char *call;
    int64_t raw_return;
    int error_number;
    int native_result;
} X4ProbeFileCall;
#define X4_PROBE_FILE_CALL_CAPACITY 64

typedef struct {
    X4ProbeFileStatus flush, sync, close;
} X4ProbeFileFinish;

enum {
    X4_PROBE_FILES_PREPARE,
    X4_PROBE_FILES_CREATE,
    X4_PROBE_FILES_WRITE,
    X4_PROBE_FILES_FLUSH,
    X4_PROBE_FILES_SYNC,
    X4_PROBE_FILES_CLOSE,
    X4_PROBE_FILES_REOPEN,
    X4_PROBE_FILES_READ,
    X4_PROBE_FILES_READ_CLOSE,
    X4_PROBE_FILES_COMPARE,
    X4_PROBE_FILES_INDEX_OPEN,
    X4_PROBE_FILES_INDEX_READ,
    X4_PROBE_FILES_INDEX_CLOSE,
    X4_PROBE_FILES_OPENAT_INFO, /* Informational, not a pass prerequisite. */
    X4_PROBE_FILES_STEP_COUNT
};
typedef struct {
    X4ProbeFileStatus step[X4_PROBE_FILES_STEP_COUNT];
    unsigned char ran[X4_PROBE_FILES_STEP_COUNT];
    char report_path[X4_PROBE_FILE_PATH_MAX];
    int passed;
    X4ProbeFileCall calls[X4_PROBE_FILE_CALL_CAPACITY];
    unsigned call_count, calls_omitted;
    int cleanup_failed;
} X4ProbeFileSelfcheck;
typedef void (*X4ProbeFilesProgress)(const X4ProbeFileSelfcheck *, void *);

/* Validated absolute paths. Reports regular/nlink1; FTP samples regular/nlink0/1.
 * No runtime-selected root, chmod or retained directory in these paths.
 * openat is reserved to the one informational idle selfcheck wrapper. */
int x4_probe_files_report_path(char *out, size_t capacity, const char *case_id,
    uint64_t sequence);
int x4_probe_files_prepare(X4ProbeFileStatus *status);
FILE *x4_probe_files_report_create(const char *path, X4ProbeFileStatus *status);
FILE *x4_probe_files_report_open(const char *path, X4ProbeFileStatus *status);
FILE *x4_probe_files_sample_open(const char *path, X4ProbeFileStatus *status);
int x4_probe_files_report_finish(FILE *file, X4ProbeFileFinish *finish);
int x4_probe_files_close(FILE *file, X4ProbeFileStatus *status);
int x4_probe_files_selfcheck(const char *report_path, const char *index_path,
    X4ProbeFileSelfcheck *result, X4ProbeFilesProgress progress, void *context);
/* No retained directory remains; preserved for shutdown callers. */
int x4_probe_files_shutdown(X4ProbeFileStatus *status);

#if defined(X4_PROBE_FILES_TESTING) && !defined(X4_OPENORBIS)
/* Offline fixtures only: omitted entirely from PS4 builds. */
enum { X4_PROBE_FILE_FAULT_NONE, X4_PROBE_FILE_FAULT_CREATE,
    X4_PROBE_FILE_FAULT_WRITE, X4_PROBE_FILE_FAULT_FLUSH,
    X4_PROBE_FILE_FAULT_SYNC, X4_PROBE_FILE_FAULT_CLOSE,
    X4_PROBE_FILE_FAULT_SHORT_WRITE, X4_PROBE_FILE_FAULT_OWNER,
    X4_PROBE_FILE_FAULT_OPENAT_INFO };
int x4_probe_files_test_root(const char *root);
void x4_probe_files_test_fault(int fault, int error_number);
/* Synthetic stat fields only, after real fstat success; never in PS4 builds. */
void x4_probe_files_test_stat(int enabled, uint32_t mode, uint64_t nlink, int64_t size);
void x4_probe_files_test_manifest(int enabled);
#endif
