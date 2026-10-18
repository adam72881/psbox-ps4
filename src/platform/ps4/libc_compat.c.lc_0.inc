// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP — getaddrinfo di origine da src/platform/ps5/libc_compat.c
// Adattato alla PS4: i simboli sono __wrap_* (ld --wrap) perché la libc
// OpenOrbis esporta già getaddrinfo, ma nella sandbox non risolve i nomi.
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

void xc_log_flush_line(const char* level, const char* msg);

#ifndef SYS_thr_self
#define SYS_thr_self 432
#endif

int timingsafe_bcmp(const void* b1, const void* b2, size_t n) {
    const unsigned char *p1 = b1, *p2 = b2;
    unsigned char diff = 0;
    while (n--) diff |= *p1++ ^ *p2++;
    return diff != 0;
}

long __wrap_syscall(long number, ...) {
    if (number == SYS_thr_self) {
        va_list ap;
        va_start(ap, number);
        long* tid = va_arg(ap, long*);
        va_end(ap);
        if (tid) *tid = (long)(uintptr_t)pthread_self();
        return 0;
    }
    errno = ENOSYS;
    return -1;
}

/* FreeBSD/Orbis: SYS_getdents è getdirentries(fd, buf, nbytes, basep).
 * musl chiama getdents con 3 argomenti: il quarto resta sporco e la
 * lettura di /data torna vuota. Qui basep è 0. */
static long xc_kcall4(long num, long a, long b, long c, long d) {
    long ret;
    unsigned char cf;
    register long r10 asm("r10") = d;
    register long r8 asm("r8") = 0;
    register long r9 asm("r9") = 0;
    asm volatile("syscall\n\tsetc %1"
                 : "=a"(ret), "=qm"(cf)
                 : "a"(num), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    if (cf || ret < 0 || (unsigned long)ret >= 0x80000000ul) {
        int e = 0;
        if (cf && ret > 0 && ret < 4096) e = (int)ret;
        else if (ret < 0 && ret > -4096) e = (int)(-ret);
        errno = e ? e : EIO;
        return -1;
    }
    return ret;
}

int __real_getdents(in