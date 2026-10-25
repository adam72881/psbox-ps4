/* SPDX-License-Identifier: GPL-3.0-only */
/* Host compilation declarations, never included in native builds. */
#pragma once
#include "../../host_stubs/orbis/libkernel.h"
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int, off_t *);
int sceKernelMapDirectMemory(void **, size_t, int, int, off_t, size_t);
int sceKernelMunmap(void *, size_t);
int sceKernelReleaseDirectMemory(off_t, size_t);
