/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_files.h"
#include <fcntl.h>
#include <unistd.h>

/* Process-time names may repeat across fresh launches. Never truncate an
 * earlier report, follow an existing link, or retry with replacement. If
 * fdopen fails, preserve the newly created empty file as a failed export. */
static inline FILE *x4_probe_report_create(const char *path)
{
#if X4_VIDEO_PROBE || defined(X4_PROBE_FILES_TESTING)
    return x4_probe_files_report_create(path, NULL);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, "wb");
    if (!file) (void)close(fd);
    return file;
#endif
}

static inline int x4_probe_report_finish(FILE *file)
{
#if X4_VIDEO_PROBE || defined(X4_PROBE_FILES_TESTING)
    return x4_probe_files_report_finish(file, NULL);
#else
    return fclose(file);
#endif
}
