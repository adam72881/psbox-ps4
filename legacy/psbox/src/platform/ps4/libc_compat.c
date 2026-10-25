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

int __real_getdents(int fd, void* buf, size_t nbytes);
int __wrap_getdents(int fd, void* buf, size_t nbytes) {
    long rc = xc_kcall4(272, fd, (long)buf, (long)nbytes, 0);
    if (rc >= 0) return (int)rc;
    int saved = errno;
    int real = __real_getdents(fd, buf, nbytes);
    static int once;
    if (!once++) {
        char msg[48];
        snprintf(msg, sizeof msg, "getdents sys %d real %d", saved, real);
        xc_log_flush_line("W", msg);
    }
    return real;
}

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <poll.h>
#include <sched.h>
#include <semaphore.h>
#include <unistd.h>
#include <orbis/libkernel.h>
#include "platform/ps4/sa_v4.h"
#include "platform/ps4/sconn_fix.h"

#include <orbis/NetCtl.h>

int xc_resolve_ipv4(const char* host, uint32_t* addr);

#define ORBIS_SO_NBIO 0x1200

/* ld --wrap: la libc resta raggiungibile per pipe, SIOCGIFCONF e il resto.
 * Stessa traduzione di xc_ioctl in src/platform/ps5/libc_compat.c. */
#include <stdio.h>

int __real_fcntl(int fd, int cmd, ...);
int __real_ioctl(int fd, unsigned long request, ...);
int __real_socket(int domain, int type, int protocol);
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
void xc_log_flush_line(const char* level, const char* msg);
void xc_log_stamp(const char* msg);
static int xc_fold_sce(int rc);

void xc_log_raw(const char* msg);

static void log_stack(size_t want, size_t before) {
    char msg[40];
    snprintf(msg, sizeof msg, "pthread %zu<-%zu", want, before);
    xc_log_flush_line("I", msg);
}

/* Il thread parte qui, prima della routine vera. Il log non usa strftime:
 * localtime legge fs:0-8, mentre il thread_local di libdatachannel è a
 * fs:0-0xa20 (modello local-exec). Se il thread muore prima di questa riga,
 * scePthreadCreate non è tornato oppure lo stack non è mappato. */
struct xc_job {
    void* (*fn)(void*);
    void* arg;
};

static void* xc_enter(void* raw) {
    struct xc_job* job = raw;
    void* (*fn)(void*) = job->fn;
    void* arg = job->arg;
    free(job);
    unsigned long tp = 0;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
    char msg[48];
    snprintf(msg, sizeof msg, "in %p fs %lx", (void*)fn, tp);
    xc_log_raw(msg);
    return fn(arg);
}

static int xc_spawn(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    struct xc_job* job = malloc(sizeof *job);
    if (!job) return __real_pthread_create(thread, attr, start, arg);
    job->fn = start;
    job->arg = arg;
    char msg[40];
    snprintf(msg, sizeof msg, "go %p", (void*)start);
    xc_log_flush_line("I", msg);
    int rc = xc_fold_sce(__real_pthread_create(thread, attr, xc_enter, job));
    snprintf(msg, sizeof msg, "rc %d", rc);
    xc_log_flush_line("I", msg);
    if (rc != 0) free(job);
    return rc;
}

/* Chi chiede già almeno 2 MiB (video, audio, worker) lo tiene. Gli altri,
 * compreso il pool di libdatachannel che passa attr NULL, restano a 2 MiB:
 * quattro stack da 8 MiB dopo i framebuffer di Videodec2 fanno morire
 * scePthreadCreate senza un codice di errore (CE-34878-0). 2 MiB bastano
 * per DTLS. */
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    const size_t kFloor = 2u << 20;
    size_t sz = 0;
    if (attr && pthread_attr_getstacksize(attr, &sz) == 0 && sz >= kFloor) {
        log_stack(sz, sz);
        return xc_spawn(thread, attr, start, arg);
    }

    pthread_attr_t big;
    if (pthread_attr_init(&big) != 0) return xc_spawn(thread, attr, start, arg);
    int detach = PTHREAD_CREATE_JOINABLE;
    if (attr) pthread_attr_getdetachstate(attr, &detach);
    pthread_attr_setdetachstate(&big, detach);
    if (pthread_attr_setstacksize(&big, kFloor) != 0) {
        pthread_attr_destroy(&big);
        xc_log_flush_line("W", "pthread: setstacksize fallito");
        return xc_spawn(thread, attr, start, arg);
    }
    log_stack(kFloor, sz);
    int rc = xc_spawn(thread, &big, start, arg);
    pthread_attr_destroy(&big);
    return rc;
}

/* libc non esporta pthread_set_name_np: il primo thread di libdatachannel
 * ci salta dentro e il processo muore con CE-34878-0. Il nome non serve. */
int pthread_set_name_np(pthread_t thread, const char* name) {
    (void)thread;
    (void)name;
    return 0;
}

/* Il pthread_rwlock di Sony non è quello di musl: l'oggetto Orbis è un
 * puntatore (inizializzatore NULL) e la seconda pthread_rwlock_init di
 * usrsctp, su Fortnite con la 01.02, uccide il processo dopo "rwinit 2".
 * Un mutex nello spazio del rwlock basta: SCTP non ha lettori in parallelo.
 * Le operazioni sotto sono tutte quelle che libusrsctp chiama. */
_Static_assert(sizeof(pthread_mutex_t) <= sizeof(pthread_rwlock_t), "mutex nel rwlock");

static pthread_mutex_t* rw_as_mutex(pthread_rwlock_t* lock) { return (pthread_mutex_t*)lock; }

int __wrap_pthread_rwlockattr_init(pthread_rwlockattr_t* attr) {
    if (attr) memset(attr, 0, sizeof *attr);
    xc_log_flush_line("I", "rwlock 0");
    return 0;
}

int __wrap_pthread_rwlockattr_destroy(pthread_rwlockattr_t* attr) {
    (void)attr;
    return 0;
}

int __wrap_pthread_rwlock_init(pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr) {
    (void)attr;
    static int n;
    int i = ++n;
    int rc = EINVAL;
    if (lock) {
        memset(lock, 0, sizeof *lock);
        rc = pthread_mutex_init(rw_as_mutex(lock), NULL);
    }
    if (i <= 4) {
        char msg[32];
        snprintf(msg, sizeof msg, "rwinit %d rc %d", i, rc);
        xc_log_flush_line("I", msg);
    }
    return rc;
}

int __wrap_pthread_rwlock_destroy(pthread_rwlock_t* lock) {
    return lock ? pthread_mutex_destroy(rw_as_mutex(lock)) : EINVAL;
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* lock) { return pthread_mutex_lock(rw_as_mutex(lock)); }

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* lock) { return pthread_mutex_lock(rw_as_mutex(lock)); }

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t* lock) { return pthread_mutex_unlock(rw_as_mutex(lock)); }

int __wrap_pthread_rwlock_tryrdlock(pthread_rwlock_t* lock) { return pthread_mutex_trylock(rw_as_mutex(lock)); }

/* L'attributo mutex di Sony è un puntatore da 8 byte; quello di musl ne ha 4.
 * Passarlo a pthread_mutex_init uccide il processo. A Sony arriva sempre NULL.
 * Il tipo ricorsivo è un mutex normale più un contatore nello slot.
 *
 * pthread_mutex_t di musl è 40 byte. scePthreadMutexInit scrive l'oggetto
 * Orbis, più grande: sulla 01.06 cinque std::recursive_mutex tornavano
 * "mtx rc 0" e il sesto passo (il callback dopo) moriva, prima di "pc body".
 * Lo scritto di Sony finiva nel membro C++ successivo. L'init va in un buffer
 * da 256 byte; lock/unlock usano quello. I mutex con PTHREAD_MUTEX_INITIALIZER
 * non passano di qui e restano sull'oggetto musl, come prima. */
int xc_log_is_ready(void);
int __real_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int __real_pthread_mutex_destroy(pthread_mutex_t* mutex);
int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t* mutex);
int __real_pthread_mutex_trylock(pthread_mutex_t* mutex);
int __real_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int __real_pthread_cond_destroy(pthread_cond_t* cond);
int __real_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int __real_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime);
int __real_pthread_cond_signal(pthread_cond_t* cond);
int __real_pthread_cond_broadcast(pthread_cond_t* cond);
int __real_pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abs);
int __real_sem_timedwait(sem_t* sem, const struct timespec* abs);
int __real_sem_wait(sem_t* sem);
int __real_nanosleep(const struct timespec* req, struct timespec* rem);
ssize_t __real_read(int fd, void* buf, size_t n);
ssize_t __real_write(int fd, const void* buf, size_t n);
ssize_t __real_recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* addr, socklen_t* len);
ssize_t __real_sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len);
int __real_connect(int fd, const struct sockaddr* addr, socklen_t len);
int __real_poll(struct pollfd* fds, nfds_t n, int timeout);
int32_t __real_scePthreadCondTimedwait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx, OrbisKernelUseconds usec);
int32_t __real_scePthreadCondWait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx);

/* Orbis risponde 0x80020000 | errno di FreeBSD (ETIMEDOUT = 60, quindi
 * 0x8002003C). libc++ di questo eboot, in condition_variable::__do_timed_wait,
 * confronta il ritorno con 0x6e (110, l'ETIMEDOUT di Linux): 60 != 110 e
 * la funzione è noexcept, quindi terminate chiama abort. I socket invece
 * restano sull'errno di musl: mbedtls confronta EAGAIN (35). */
enum { XC_LIBCXX_ETIMEDOUT = 110 };

static int xc_fold_sce(int rc) {
    unsigned u = (unsigned)rc;
    if ((u & 0xFFFF0000u) == 0x80020000u) {
        int e = (int)(u & 0xFFFFu);
        return e == 0 ? EINVAL : e;
    }
    return rc;
}

static int xc_is_timeout(int e) { return e == 60 || e == XC_LIBCXX_ETIMEDOUT || e == 9938; }

/* Ritorno di pthread_cond_timedwait: 0 resta 0, il timeout diventa 110,
 * EINTR è un risveglio spurio (altrimenti libc++ abortisce). */
static int xc_cv_timed(int rc) {
    unsigned raw = (unsigned)rc;
    int e = xc_fold_sce(rc);
    if (e == 0) return 0;
    if (xc_is_timeout(e)) {
        static int once;
        if (!once++) {
            char msg[48];
            snprintf(msg, sizeof msg, "cond timeout 0x%x -> 110", raw);
            xc_log_stamp(msg);
        }
        return XC_LIBCXX_ETIMEDOUT;
    }
    if (e == EINTR) return 0;
    static int n;
    if (n < 4) {
        char msg[48];
        snprintf(msg, sizeof msg, "cond rc 0x%x -> %d", raw, e);
        xc_log_stamp(msg);
        ++n;
    }
    return e;
}

static int xc_cv_wait(int rc) {
    int e = xc_fold_sce(rc);
    if (e == 0 || e == EINTR || xc_is_timeout(e)) return 0;
    return e;
}

static int xc_lock_raw(pthread_mutex_t* raw) {
    int spins = 0, rc;
    do {
        rc = xc_fold_sce(__real_pthread_mutex_lock(raw));
    } while (rc == EINTR && ++spins < 16);
    return rc;
}

static void xc_fold_errno(void) { errno = xc_fold_sce(errno); }

static int xc_sys_int(int rc) {
    if (rc == 0) return 0;
    if (rc < 0) {
        xc_fold_errno();
        return -1;
    }
    unsigned u = (unsigned)rc;
    if ((u & 0xFFFF0000u) == 0x80020000u) {
        errno = xc_fold_sce(rc);
        return -1;
    }
    return rc;
}

static ssize_t xc_sys_sz(ssize_t rc) {
    if (rc >= 0) return rc;
    xc_fold_errno();
    return -1;
}

#define XC_SONY_OBJ 256
/* Il PeerConnection, usrsctp, libjuice e mbedtls insieme superano 128 mutex.
 * Ogni slot è 256 byte: 512 mutex e 128 cond restano sotto i 200 KB. */
#define XC_MTX_MAX 512
#define XC_COND_MAX 128

struct xc_slot {
    void* key;
    int rec;
    int depth;
    pthread_t owner;
    _Alignas(16) unsigned char raw[XC_SONY_OBJ];
};

static struct xc_slot g_mtx[XC_MTX_MAX];
static struct xc_slot g_cond[XC_COND_MAX];

static struct xc_slot* slot_find(struct xc_slot* tab, int n, void* key) {
    for (int i = 0; i < n; i++) {
        if (__atomic_load_n(&tab[i].key, __ATOMIC_ACQUIRE) == key) return &tab[i];
    }
    return NULL;
}

static struct xc_slot* slot_take(struct xc_slot* tab, int n, void* key) {
    struct xc_slot* have = slot_find(tab, n, key);
    if (have) {
        have->rec = 0;
        have->depth = 0;
        have->owner = 0;
        memset(have->raw, 0, sizeof have->raw);
        return have;
    }
    for (int i = 0; i < n; i++) {
        void* expected = NULL;
        if (__atomic_compare_exchange_n(&tab[i].key, &expected, key, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            tab[i].rec = 0;
            tab[i].depth = 0;
            tab[i].owner = 0;
            memset(tab[i].raw, 0, sizeof tab[i].raw);
            return &tab[i];
        }
    }
    return NULL;
}

static void slot_drop(struct xc_slot* tab, int n, void* key) {
    struct xc_slot* s = slot_find(tab, n, key);
    if (!s) return;
    s->rec = 0;
    s->depth = 0;
    s->owner = 0;
    __atomic_store_n(&s->key, (void*)0, __ATOMIC_RELEASE);
}

static pthread_mutex_t* mtx_sony(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    return s ? (pthread_mutex_t*)s->raw : mutex;
}

static pthread_cond_t* cond_sony(pthread_cond_t* cond) {
    struct xc_slot* s = cond ? slot_find(g_cond, XC_COND_MAX, cond) : NULL;
    return s ? (pthread_cond_t*)s->raw : cond;
}

int __wrap_pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    if (attr) memset(attr, 0, sizeof *attr);
    return 0;
}

int __wrap_pthread_mutexattr_destroy(pthread_mutexattr_t* attr) {
    (void)attr;
    return 0;
}

int __wrap_pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
    if (!attr) return EINVAL;
    attr->__attr = (unsigned)type;
    return 0;
}

int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    static int n;
    unsigned type = attr ? attr->__attr : 0;
    int rec = type == (unsigned)PTHREAD_MUTEX_RECURSIVE;
    int logged = 0;
    if (!mutex) return EINVAL;
    if (xc_log_is_ready() && n < 40) {
        char msg[32];
        snprintf(msg, sizeof msg, "mtx %s %d", rec ? "rec" : "std", ++n);
        xc_log_flush_line("I", msg);
        logged = 1;
    }
    struct xc_slot* s = slot_take(g_mtx, XC_MTX_MAX, mutex);
    if (!s) {
        if (xc_log_is_ready()) xc_log_flush_line("W", "mtx pieno");
        return ENOMEM;
    }
    s->rec = rec;
    memset(mutex, 0, sizeof *mutex);
    int rc = xc_fold_sce(__real_pthread_mutex_init((pthread_mutex_t*)s->raw, NULL));
    if (logged) {
        char msg[24];
        snprintf(msg, sizeof msg, "mtx rc %d", rc);
        xc_log_flush_line("I", msg);
    }
    if (rc != 0) slot_drop(g_mtx, XC_MTX_MAX, mutex);
    return rc;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    struct xc_slot* s = slot_find(g_mtx, XC_MTX_MAX, mutex);
    if (!s) return __real_pthread_mutex_destroy(mutex);
    int rc = xc_fold_sce(__real_pthread_mutex_destroy((pthread_mutex_t*)s->raw));
    slot_drop(g_mtx, XC_MTX_MAX, mutex);
    return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (!s) return xc_lock_raw(mutex);
    if (s->rec && s->depth > 0 && pthread_equal(s->owner, pthread_self())) {
        s->depth++;
        return 0;
    }
    int rc = xc_lock_raw((pthread_mutex_t*)s->raw);
    if (rc != 0) return rc;
    if (s->rec) {
        s->owner = pthread_self();
        s->depth = 1;
    }
    return 0;
}

int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (!s) return xc_fold_sce(__real_pthread_mutex_trylock(mutex));
    if (s->rec && s->depth > 0 && pthread_equal(s->owner, pthread_self())) {
        s->depth++;
        return 0;
    }
    int rc = xc_fold_sce(__real_pthread_mutex_trylock((pthread_mutex_t*)s->raw));
    if (rc != 0) return rc;
    if (s->rec) {
        s->owner = pthread_self();
        s->depth = 1;
    }
    return 0;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (!s) return __real_pthread_mutex_unlock(mutex);
    if (s->rec && s->depth > 1 && pthread_equal(s->owner, pthread_self())) {
        s->depth--;
        return 0;
    }
    s->depth = 0;
    return xc_fold_sce(__real_pthread_mutex_unlock((pthread_mutex_t*)s->raw));
}

int __wrap_pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abs) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (s && s->rec && s->depth > 0 && pthread_equal(s->owner, pthread_self())) {
        s->depth++;
        return 0;
    }
    pthread_mutex_t* raw = s ? (pthread_mutex_t*)s->raw : mutex;
    int spins = 0, rc;
    do {
        rc = xc_fold_sce(__real_pthread_mutex_timedlock(raw, abs));
    } while (rc == EINTR && ++spins < 8);
    if (rc == EINTR || xc_is_timeout(rc)) rc = XC_LIBCXX_ETIMEDOUT;
    if (rc != 0) return rc;
    if (s && s->rec) {
        s->owner = pthread_self();
        s->depth = 1;
    }
    return 0;
}

int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    static int n;
    int logged = 0;
    (void)attr;
    if (!cond) return EINVAL;
    if (xc_log_is_ready() && n < 16) {
        char msg[24];
        snprintf(msg, sizeof msg, "cond %d", ++n);
        xc_log_flush_line("I", msg);
        logged = 1;
    }
    struct xc_slot* s = slot_take(g_cond, XC_COND_MAX, cond);
    if (!s) {
        if (xc_log_is_ready()) xc_log_flush_line("W", "cond pieno");
        return ENOMEM;
    }
    memset(cond, 0, sizeof *cond);
    int rc = xc_fold_sce(__real_pthread_cond_init((pthread_cond_t*)s->raw, NULL));
    if (logged) {
        char msg[24];
        snprintf(msg, sizeof msg, "cond rc %d", rc);
        xc_log_flush_line("I", msg);
    }
    if (rc != 0) slot_drop(g_cond, XC_COND_MAX, cond);
    return rc;
}

int __wrap_pthread_cond_destroy(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    struct xc_slot* s = slot_find(g_cond, XC_COND_MAX, cond);
    if (!s) return __real_pthread_cond_destroy(cond);
    int rc = __real_pthread_cond_destroy((pthread_cond_t*)s->raw);
    slot_drop(g_cond, XC_COND_MAX, cond);
    return rc;
}

int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    return xc_cv_wait(__real_pthread_cond_wait(cond_sony(cond), mtx_sony(mutex)));
}

int __wrap_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    return xc_cv_timed(__real_pthread_cond_timedwait(cond_sony(cond), mtx_sony(mutex), abstime));
}

/* musl numera gli orologi come Linux (MONOTONIC = 1). Il kernel Orbis è
 * FreeBSD: 1 è CLOCK_VIRTUAL, il tempo CPU del processo, che avanza circa
 * 23 volte più piano del muro. Il monotonic vero è 4. Un'attesa di 1 s
 * sul clock 1 tornava dopo ~23 s di orologio da parete: il ClientHello
 * veniva ritrasmesso quando il server aveva già chiuso il DTLS. */
static clockid_t xc_orbis_clock(clockid_t clk) {
    switch ((int)clk) {
    case 0: return 0; /* REALTIME */
    case 1: return 4; /* linux MONOTONIC -> freebsd MONOTONIC */
    case 2: return 2;
    case 3: return 3;
    case 4: return 4; /* MONOTONIC_RAW linux, e il monotonic freebsd */
    case 5: return 0; /* REALTIME_COARSE */
    case 6: return 4; /* MONOTONIC_COARSE */
    case 7: return 4; /* BOOTTIME */
    default: return (clockid_t)4;
    }
}

/* libc++ con CLOCK_MONOTONIC chiama questa, che su Orbis non c'è.
 * Il tempo assoluto monotonic si traduce in tempo reale e si usa timedwait. */
int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t clk, const struct timespec* abstime) {
    struct timespec rt;
    const struct timespec* ts = abstime;
    if (abstime && clk != CLOCK_REALTIME) {
        struct timespec now, real;
        if (clock_gettime(clk, &now) == 0 && clock_gettime(CLOCK_REALTIME, &real) == 0) {
            long sec = (long)abstime->tv_sec - (long)now.tv_sec;
            long nsec = abstime->tv_nsec - now.tv_nsec;
            if (nsec < 0) {
                sec--;
                nsec += 1000000000L;
            }
            rt.tv_sec = real.tv_sec + sec;
            rt.tv_nsec = real.tv_nsec + nsec;
            if (rt.tv_nsec >= 1000000000L) {
                rt.tv_sec++;
                rt.tv_nsec -= 1000000000L;
            }
            ts = &rt;
        }
    }
    return __wrap_pthread_cond_timedwait(cond, mutex, ts);
}

int __wrap_pthread_cond_signal(pthread_cond_t* cond) {
    return xc_fold_sce(__real_pthread_cond_signal(cond_sony(cond)));
}

int __wrap_pthread_cond_broadcast(pthread_cond_t* cond) {
    return xc_fold_sce(__real_pthread_cond_broadcast(cond_sony(cond)));
}

int __wrap_scePthreadCondTimedwait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx, OrbisKernelUseconds usec) {
    return xc_cv_timed(__real_scePthreadCondTimedwait(cond, mtx, usec));
}

int __wrap_scePthreadCondWait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx) {
    return xc_cv_wait(__real_scePthreadCondWait(cond, mtx));
}

void __real_usrsctp_init(uint16_t port, int (*output)(void*, void*, size_t, uint8_t, uint8_t),
                         void (*debug)(const char*, ...));

void __wrap_usrsctp_init(uint16_t port, int (*output)(void*, void*, size_t, uint8_t, uint8_t),
                         void (*debug)(const char*, ...)) {
    xc_log_flush_line("I", "usrsctp");
    __real_usrsctp_init(port, output, debug);
    xc_log_flush_line("I", "usrsctp ok");
}

void* __real_juice_create(const void* config);
void* __wrap_juice_create(const void* config) {
    xc_log_flush_line("I", "juice");
    void* agent = __real_juice_create(config);
    xc_log_flush_line("I", agent ? "juice ok" : "juice no");
    return agent;
}

void __real_mbedtls_entropy_init(void* ctx);
void __wrap_mbedtls_entropy_init(void* ctx) {
    xc_log_flush_line("I", "entropy");
    __real_mbedtls_entropy_init(ctx);
    xc_log_flush_line("I", "entropy ok");
}

int __real_mbedtls_ctr_drbg_seed(void* ctx, int (*f_entropy)(void*, unsigned char*, size_t), void* p_entropy,
                                 const unsigned char* custom, size_t len);
int __wrap_mbedtls_ctr_drbg_seed(void* ctx, int (*f_entropy)(void*, unsigned char*, size_t), void* p_entropy,
                                 const unsigned char* custom, size_t len) {
    xc_log_flush_line("I", "rand");
    xc_log_flush_line("I", "drbg");
    int rc = __real_mbedtls_ctr_drbg_seed(ctx, f_entropy, p_entropy, custom, len);
    char msg[24];
    snprintf(msg, sizeof msg, "drbg %d", rc);
    xc_log_flush_line("I", msg);
    return rc;
}

/* usrsctp, compilato come FreeBSD, apre AF_ROUTE e SOCK_RAW. Su Orbis quel
 * socket non esiste. getifaddrs di musl usa netlink (AF_NETLINK + SOCK_RAW):
 * lo stesso buco. I dati SCTP di WebRTC passano dal callback, non dal kernel. */
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
int __wrap_socket(int domain, int type, int protocol) {
    int kind = type & 0xf;
    /* Orbis non ha il dual-stack: IPV6_V6ONLY risponde e42 e sendto fam 28
     * torna EINVAL. libjuice deve aprire solo AF_INET. */
    if (domain == AF_INET6) {
        static int once;
        if (!once++) xc_log_flush_line("W", "socket: niente ipv6, solo AF_INET");
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (domain == AF_ROUTE || domain == AF_UNSPEC || domain == AF_NETLINK || kind == SOCK_RAW) {
        char msg[40];
        snprintf(msg, sizeof msg, "socket no %d/%d/%d", domain, type, protocol);
        xc_log_flush_line("W", msg);
        errno = EPROTONOSUPPORT;
        return -1;
    }
    int fd = __real_socket(domain, type, protocol);
    if (fd < 0) xc_fold_errno();
    /* RCVBUF Orbis: lo imposta juice (PSBOX_RCVBUF) e lo remap __wrap_setsockopt
     * (8→0x1002). Niente drain: sulla 01.25 rubava i pacchetti a juice. */
    static int n;
    if (xc_log_is_ready() && n < 12) {
        char msg[48];
        snprintf(msg, sizeof msg, "socket %d/%d/%d => %d", domain, kind, protocol, fd);
        xc_log_flush_line("I", msg);
        ++n;
    }
    return fd;
}

int __wrap_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    uintptr_t arg = va_arg(ap, uintptr_t);
    va_end(ap);
    int rc = __real_fcntl(fd, cmd, arg);
    /* La sandbox rifiuta O_NONBLOCK sui socket (come sulla PS5). SO_NBIO sì.
     * Sulle pipe setsockopt fallisce e resta il fcntl reale. */
    if (cmd == F_SETFL && ((int)arg & O_NONBLOCK)) {
        int on = 1;
        if (setsockopt(fd, SOL_SOCKET, ORBIS_SO_NBIO, &on, sizeof on) == 0) return 0;
    }
    return rc;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (request == FIONBIO && arg) {
        int on = *(const int*)arg ? 1 : 0;
        if (setsockopt(fd, SOL_SOCKET, ORBIS_SO_NBIO, &on, sizeof on) == 0) return 0;
    }
    /* SIOCGIF* di Linux (0x89xx) non è un ioctl di Orbis: passarlo al kernel
     * è il passo che fa morire getifaddrs/MTU. MTU finto 1500, il resto no. */
    if (request == SIOCGIFMTU && arg) {
        struct ifreq* ifr = arg;
        ifr->ifr_mtu = 1500;
        static int once;
        if (!once++) xc_log_flush_line("I", "ioctl mtu 1500");
        return 0;
    }
    if (request == SIOCGIFCONF || request == SIOCGIFADDR || request == SIOCGIFFLAGS ||
        request == SIOCGIFNETMASK || request == SIOCGIFINDEX) {
        static int n;
        if (n < 4) {
            char msg[32];
            snprintf(msg, sizeof msg, "ioctl if %lx", request);
            xc_log_flush_line("W", msg);
            ++n;
        }
        errno = ENOTTY;
        return -1;
    }
    return __real_ioctl(fd, request, arg);
}

static struct addrinfo* make_ai(int family, const void* addr, unsigned scope, int port, const struct addrinfo* hints) {
    size_t salen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    struct addrinfo* ai = calloc(1, sizeof *ai + salen);
    if (!ai) return NULL;
    struct sockaddr* sa = (struct sockaddr*)(ai + 1);
    if (family == AF_INET) {
        struct sockaddr_in* s4 = (struct sockaddr_in*)sa;
        s4->sin_len = sizeof *s4;
        s4->sin_family = AF_INET;
        s4->sin_port = htons((uint16_t)port);
        memcpy(&s4->sin_addr, addr, 4);
    } else {
        struct sockaddr_in6* s6 = (struct sockaddr_in6*)sa;
        s6->sin6_len = sizeof *s6;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((uint16_t)port);
        s6->sin6_scope_id = scope;
        memcpy(&s6->sin6_addr, addr, 16);
    }
    ai->ai_family = family;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_flags = hints ? hints->ai_flags : 0;
    ai->ai_addrlen = (socklen_t)salen;
    ai->ai_addr = sa;
    return ai;
}

int __wrap_getaddrinfo(const char* host, const char* serv, const struct addrinfo* hints, struct addrinfo** res) {
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int flags = hints ? hints->ai_flags : 0;
    int port = 0;
    if (!res) return EAI_FAIL;
    *res = NULL;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return EAI_FAMILY;
    if (serv && *serv) {
        char* end = NULL;
        long p = strtol(serv, &end, 10);
        if (*end || p < 0 || p > 65535) {
            if (!strcmp(serv, "https")) p = 443;
            else if (!strcmp(serv, "http")) p = 80;
            else return EAI_SERVICE;
        }
        port = (int)p;
    }

    unsigned char a[16];
    if (family == AF_INET6) return EAI_FAMILY;
    if (!host) {
        /* Solo IPv4: un risultato AF_INET6 fa aprire a juice un socket che
         * poi manda ogni STUN come fam 28. */
        uint32_t v4 = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
        return (*res = make_ai(AF_INET, &v4, 0, port, hints)) ? 0 : EAI_MEMORY;
    }
    if (inet_pton(AF_INET, host, a) == 1)
        return (*res = make_ai(AF_INET, a, 0, port, hints)) ? 0 : EAI_MEMORY;
    if (flags & AI_NUMERICHOST) return EAI_NONAME;
    if (family == AF_INET6) return EAI_NONAME;
    uint32_t v4 = 0;
    if (xc_resolve_ipv4(host, &v4) != 0) return EAI_NONAME;
    return (*res = make_ai(AF_INET, &v4, 0, port, hints)) ? 0 : EAI_MEMORY;
}

void __wrap_freeaddrinfo(struct addrinfo* ai) {
    while (ai) {
        struct addrinfo* next = ai->ai_next;
        free(ai);
        ai = next;
    }
}

int __wrap_getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, size_t hostlen, char* serv,
                       size_t servlen, int flags) {
    (void)salen;
    (void)flags;
    int port = 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* s4 = (const struct sockaddr_in*)sa;
        if (host && hostlen && !inet_ntop(AF_INET, &s4->sin_addr, host, (socklen_t)hostlen)) return EAI_OVERFLOW;
        port = ntohs(s4->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* s6 = (const struct sockaddr_in6*)sa;
        if (host && hostlen && !inet_ntop(AF_INET6, &s6->sin6_addr, host, (socklen_t)hostlen)) return EAI_OVERFLOW;
        port = ntohs(s6->sin6_port);
    } else {
        return EAI_FAMILY;
    }
    if (serv && servlen) snprintf(serv, servlen, "%d", port);
    return 0;
}

/* usrsctp, sul percorso FreeBSD, chiama arc4random_buf. La libc musl non lo ha. */
int ps4_random_bytes(void* out, size_t len);

void arc4random_buf(void* buf, size_t n) {
    if (!buf || n == 0) return;
    if (!ps4_random_bytes(buf, n)) memset(buf, 0, n);
}

/* libc++ chiama __cxa_thread_atexit_impl, che musl di OpenOrbis non esporta.
 * I distruttori thread_local girano alla fine del thread, come su FreeBSD. */
struct xc_tls_dtor {
    void (*dtor)(void*);
    void* obj;
    struct xc_tls_dtor* next;
};

static pthread_key_t xc_tls_key;
static pthread_once_t xc_tls_once = PTHREAD_ONCE_INIT;

static void xc_run_tls_dtors(void* p) {
    struct xc_tls_dtor* n = p;
    while (n) {
        struct xc_tls_dtor* next = n->next;
        n->dtor(n->obj);
        free(n);
        n = next;
    }
}

static void xc_tls_key_init(void) { pthread_key_create(&xc_tls_key, xc_run_tls_dtors); }

int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* dso) {
    struct xc_tls_dtor* n;
    (void)dso;
    if (!dtor) return 0;
    static int once;
    if (!once++) xc_log_flush_line("I", "tls dtor");
    pthread_once(&xc_tls_once, xc_tls_key_init);
    n = malloc(sizeof *n);
    if (!n) return -1;
    n->dtor = dtor;
    n->obj = obj;
    n->next = pthread_getspecific(xc_tls_key);
    pthread_setspecific(xc_tls_key, n);
    return 0;
}

/* Tabella di musl (sysconf.values), copiata così com'è. I valori >= -1 si
 * restituiscono. Quelli più piccoli sono casi calcolati: in musl gli indici
 * 5 e 6 (nomi 83 e 84, i processori) chiamano cpuset_getaffinity, che su
 * Orbis non esiste. Quel .o non viene linkato: qui il risultato è 4. */
static const short xc_sysconf_values[] = {
      -254,-32762,   100,    32,-32761,    -1,     6,     1,     1,  -255,    -1,  -255,
      -255,    -1,    -1,  -255,  -255,  -255,  -255,  -255,  -255,  -255,  -255,    -1,
        -1,  -246,  -245,    -1,  -253,  -255,  -252,    30,   256,  -251,    -1,    -1,
        99,  2048,    99,  1000,     2,     0,    -1,    -1,   255,     0,  -255,  -255,
        -1,    -1,    -1,    -1,    -1,     0,     0,     0,     0,     0,     0,     0,
      1024,     0,     0,     0,     0,     0,     0,  -255,  -255,    -1,    -1,   256,
        32,     4,   128,  2048,    -1,  -255,  -255,  -255,    -1,    -1,  -255,  -250,
      -249,  -248,  -247,    -1,    -1,   700,   700,     1,    -1,     1,     1,    -1,
         0,    -1,    -1,    -1,    -1,     0,     0,     0,     0,     0,     0,     0,
         0,    20,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
         0,     0,     0,     0,     0,    -1,    -1,     1,    -1,    -1,    -1,    -1,
      -255,  -255,     0,     0,     0,  -255,  -255,  -255,     0,     0,     0,     0,
         0,     0,     0,     0,     0,  -255,     0,     0,     0,  -255,  -255,     1,
         0,     1,     0,  -255,    -1,    -1,     0,     0,  -255,    -1,     0,     0,
        -1,    -1,    -1,    -1,    -1,    40,  -246,    -1,    -1,    -1,     1,    -1,
       255,    -1,    -1,    -1,    -1,     0,     0,     0,     0,     0,     0,     0,
         0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
         0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
         0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
         0,     0,     0,     0,     0,     0,     0,  -255,  -255,    -1,    -1,     1,
        -1,    -1,    -1,    -1,    -1,    -1,  -246,    -1,    -1,
};
_Static_assert(sizeof xc_sysconf_values / sizeof xc_sysconf_values[0] == 249, "tabella sysconf");

static long xc_phys_pages(void) {
    size_t bytes = sceKernelGetDirectMemorySize();
    if (!bytes) return 262144; /* 4 GiB / 16 KiB */
    return (long)(bytes >> 14);
}

long sysconf(int name) {
    if (name < 0 || (size_t)name >= sizeof xc_sysconf_values / sizeof xc_sysconf_values[0]) {
        errno = EINVAL;
        return -1;
    }
    int v = xc_sysconf_values[name];
    if (v >= -1) return v;
    if (name == 1 || name == 4) {
        struct rlimit rl;
        if (getrlimit(v & 0x3fff, &rl) != 0) return -1;
        if (rl.rlim_cur == RLIM_INFINITY) return -1;
        return (long)rl.rlim_cur;
    }
    switch (((unsigned)v & 0xffu) - 1u) {
    case 0: return 0x31069;          /* _SC_ARG_MAX e simili, costante musl */
    case 1: return 0x20000;
    case 2: return 0x8000;
    case 3: return 0x4000;           /* _SC_PAGESIZE: 16 KiB */
    case 4: return 0x7fffffff;
    case 5:                          /* _SC_NPROCESSORS_CONF: era cpuset_getaffinity */
    case 6: {                        /* _SC_NPROCESSORS_ONLN */
        static int once;
        if (!once++) xc_log_flush_line("I", "sysconf nproc 4");
        return 4;
    }
    case 7:                          /* _SC_PHYS_PAGES */
    case 8: {                        /* _SC_AVPHYS_PAGES: musl usa sysinfo */
        static int once;
        if (!once++) xc_log_flush_line("I", "sysconf pages");
        return xc_phys_pages();
    }
    case 9: return 0;
    case 10: return 0x7fffffff;
    default: {
        static int n;
        if (n < 6) {
            char msg[24];
            snprintf(msg, sizeof msg, "sysconf %d", name);
            xc_log_flush_line("I", msg);
            ++n;
        }
        errno = EINVAL;
        return -1;
    }
    }
}

static void cpu_mask(void* mask, size_t setsize) {
    if (!mask || !setsize) return;
    memset(mask, 0, setsize);
    ((unsigned char*)mask)[0] = 0x0f;
}

int __wrap_sched_getaffinity(int pid, size_t setsize, void* mask) {
    (void)pid;
    static int once;
    if (!once++) xc_log_flush_line("I", "sched_getaffinity");
    cpu_mask(mask, setsize);
    return 0;
}

int __wrap_sched_setaffinity(int pid, size_t setsize, const void* mask) {
    (void)pid;
    (void)setsize;
    (void)mask;
    static int once;
    if (!once++) xc_log_flush_line("I", "sched_setaffinity");
    return 0;
}

int __wrap_pthread_getaffinity_np(pthread_t thread, size_t setsize, void* mask) {
    (void)thread;
    static int once;
    if (!once++) xc_log_flush_line("I", "pthread_getaffinity");
    cpu_mask(mask, setsize);
    return 0;
}

int __wrap_pthread_setaffinity_np(pthread_t thread, size_t setsize, const void* mask) {
    (void)thread;
    (void)setsize;
    (void)mask;
    static int once;
    if (!once++) xc_log_flush_line("I", "pthread_setaffinity");
    return 0;
}

/* getifaddrs di musl è netlink. Su Orbis non c'è: ICE si fermerebbe senza
 * un indirizzo locale. L'IP della console arriva da sceNetCtl, già avviato
 * in platform::init. */
struct xc_ifa {
    struct ifaddrs ifa;
    struct sockaddr_in addr;
    struct sockaddr_in mask;
    char name[8];
};

int __wrap_getifaddrs(struct ifaddrs** out) {
    OrbisNetCtlInfo ip;
    OrbisNetCtlInfo mask;
    char msg[48];
    struct xc_ifa* n;
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    memset(&ip, 0, sizeof ip);
    memset(&mask, 0, sizeof mask);
    int rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &ip);
    int rc2 = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_NETMASK, &mask);
    snprintf(msg, sizeof msg, "ifaddr %d %s", rc, ip.ip_address);
    xc_log_flush_line(rc < 0 ? "W" : "I", msg);
    if (rc < 0 || ip.ip_address[0] == 0) {
        errno = ENETDOWN;
        return -1;
    }
    n = calloc(1, sizeof *n);
    if (!n) return -1;
    memcpy(n->name, "eth0", 5);
    n->ifa.ifa_next = NULL;
    n->ifa.ifa_name = n->name;
    n->ifa.ifa_flags = IFF_UP | IFF_RUNNING;
    n->addr.sin_len = sizeof n->addr;
    n->addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip.ip_address, &n->addr.sin_addr) != 1) {
        free(n);
        errno = EFAULT;
        return -1;
    }
    n->ifa.ifa_addr = (struct sockaddr*)&n->addr;
    if (rc2 == 0 && inet_pton(AF_INET, mask.ip_address, &n->mask.sin_addr) == 1) {
        n->mask.sin_len = sizeof n->mask;
        n->mask.sin_family = AF_INET;
        n->ifa.ifa_netmask = (struct sockaddr*)&n->mask;
    }
    *out = &n->ifa;
    return 0;
}

void __wrap_freeifaddrs(struct ifaddrs* ifa) { free(ifa); }

unsigned __wrap_if_nametoindex(const char* name) {
    char msg[40];
    snprintf(msg, sizeof msg, "ifindex %s", name ? name : "-");
    xc_log_flush_line("I", msg);
    return 1;
}

char* __wrap_if_indextoname(unsigned index, char* name) {
    char msg[32];
    snprintf(msg, sizeof msg, "ifname %u", index);
    xc_log_flush_line("I", msg);
    if (!name) {
        errno = EFAULT;
        return NULL;
    }
    memcpy(name, "eth0", 5);
    return name;
}

/* getentropy di musl è ENOSYS. mbedtls non la usa (hardware poll), libjuice
 * e un eventuale /dev/urandom sì. I byte sono gli stessi di entropy_ps4.c. */
int ps4_random_bytes(void* out, size_t len);

int __wrap_getentropy(void* buf, size_t len) {
    static int once;
    if (!once++) xc_log_flush_line("I", "getentropy");
    if (!buf || len > 256) {
        errno = EIO;
        return -1;
    }
    if (!ps4_random_bytes(buf, len)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

ssize_t __wrap_getrandom(void* buf, size_t len, unsigned flags) {
    static int once;
    (void)flags;
    if (!once++) xc_log_flush_line("I", "getrandom");
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (!ps4_random_bytes(buf, len)) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)len;
}

long __wrap_random(void) {
    unsigned char b[4];
    static int once;
    if (!once++) xc_log_flush_line("I", "random");
    if (!ps4_random_bytes(b, sizeof b)) return 1;
    return (long)(b[0] | ((unsigned)b[1] << 8) | ((unsigned)(b[2] & 0x7f) << 16));
}

void __wrap_srandom(unsigned seed) {
    (void)seed;
    static int once;
    if (!once++) xc_log_flush_line("I", "srandom");
}

/* getauxval su questa libc legge un auxv che l'eboot non ha. mbedtls su
 * x86 non lo usa (cpuid), ma un ritorno 0 tiene spenta ogni HWCAP. */
unsigned long __wrap_getauxval(unsigned long type) {
    static int n;
    if (n < 4) {
        char msg[32];
        snprintf(msg, sizeof msg, "getauxval %lu", type);
        xc_log_flush_line("I", msg);
        ++n;
    }
    return 0;
}

/* prctl è Linux. Su questo target libdatachannel usa pthread_set_name_np,
 * già un no-op. Se qualcosa chiama prctl, non si fa la syscall. */
#ifndef PR_SET_NAME
#define PR_SET_NAME 15
#endif
int __wrap_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5) {
    static int once;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    if (!once++) {
        char msg[24];
        snprintf(msg, sizeof msg, "prctl %d", option);
        xc_log_flush_line("I", msg);
    }
    if (option == PR_SET_NAME) return 0;
    errno = ENOSYS;
    return -1;
}

/* CLOCK_BOOTTIME e MONOTONIC_RAW non sono orologi Orbis. Il monotonic del
 * kernel è l'id 4, non l'1 di Linux. */
int __real_clock_gettime(clockid_t clk, struct timespec* ts);

int __wrap_clock_gettime(clockid_t clk, struct timespec* ts) {
    if (!ts) {
        errno = EINVAL;
        return -1;
    }
    clockid_t id = xc_orbis_clock(clk);
    int rc = xc_sys_int(__real_clock_gettime(id, ts));
    if (rc != 0 && id != 0) rc = xc_sys_int(__real_clock_gettime(0, ts));
    if (rc != 0) {
        /* Microsecondi di muro dall'avvio del processo: stesso passo del
         * realtime, non il tempo CPU. */
        uint64_t us = sceKernelGetProcessTime();
        ts->tv_sec = (time_t)(us / 1000000ull);
        ts->tv_nsec = (long)((us % 1000000ull) * 1000ull);
        rc = 0;
    }
    if (clk == 1 || clk == CLOCK_MONOTONIC) {
        static int once;
        if (!once++) {
            char msg[72];
            snprintf(msg, sizeof msg, "clock mono %d->%d %ld.%03ld", (int)clk, (int)id, (long)ts->tv_sec,
                     (long)(ts->tv_nsec / 1000000L));
            xc_log_flush_line("I", msg);
        }
    }
    return rc;
}

/* /proc e /sys non ci sono. /dev/urandom sì (via pipe + ps4_random_bytes):
 * mbedtls/usrsctp a freddo lo aprono e un ENOENT lascia DTLS senza entropy. */
static int blocked_path(const char* path) {
    if (!path) return 0;
    if (strncmp(path, "/proc", 5) == 0 || strncmp(path, "/sys", 4) == 0) return 1;
    return 0;
}

static int xc_is_urandom(const char* path) {
    return path && (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0);
}

enum { XC_RAND_FD_MAX = 8 };
static int g_rand_fds[XC_RAND_FD_MAX];
static int g_rand_n;
static pthread_mutex_t g_rand_mu = PTHREAD_MUTEX_INITIALIZER;

static void xc_rand_add(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_rand_mu);
    if (g_rand_n < XC_RAND_FD_MAX) g_rand_fds[g_rand_n++] = fd;
    pthread_mutex_unlock(&g_rand_mu);
}

static int xc_rand_has(int fd) {
    int i, found = 0;
    pthread_mutex_lock(&g_rand_mu);
    for (i = 0; i < g_rand_n; ++i)
        if (g_rand_fds[i] == fd) {
            found = 1;
            break;
        }
    pthread_mutex_unlock(&g_rand_mu);
    return found;
}

static void xc_rand_del(int fd) {
    int i;
    pthread_mutex_lock(&g_rand_mu);
    for (i = 0; i < g_rand_n; ++i) {
        if (g_rand_fds[i] != fd) continue;
        g_rand_fds[i] = g_rand_fds[--g_rand_n];
        break;
    }
    pthread_mutex_unlock(&g_rand_mu);
}

static int xc_open_urandom(void) {
    int p[2];
    static int once;
    if (!once++) xc_log_flush_line("I", "open /dev/urandom -> entropy PS4");
    if (pipe(p) != 0) {
        errno = EIO;
        return -1;
    }
    close(p[1]);
    xc_rand_add(p[0]);
    return p[0];
}

static void log_blocked(const char* what, const char* path) {
    char msg[72];
    snprintf(msg, sizeof msg, "%s %s", what, path ? path : "-");
    xc_log_flush_line("W", msg);
}

int __real_open(const char* path, int flags, ...);
FILE* __real_fopen(const char* path, const char* mode);
int __real_close(int fd);

int __wrap_open(const char* path, int flags, ...) {
    va_list ap;
    int mode;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    if (xc_is_urandom(path)) return xc_open_urandom();
    if (blocked_path(path)) {
        log_blocked("open", path);
        errno = ENOENT;
        return -1;
    }
    return __real_open(path, flags, mode);
}

FILE* __wrap_fopen(const char* path, const char* mode) {
    if (xc_is_urandom(path)) {
        int fd = xc_open_urandom();
        if (fd < 0) return NULL;
        FILE* f = fdopen(fd, mode && mode[0] ? mode : "rb");
        if (!f) {
            xc_rand_del(fd);
            close(fd);
        }
        return f;
    }
    if (blocked_path(path)) {
        log_blocked("fopen", path);
        errno = ENOENT;
        return NULL;
    }
    return __real_fopen(path, mode);
}

int __wrap_close(int fd) {
    xc_rand_del(fd);
    return __real_close(fd);
}

int __real_setsockopt(int fd, int level, int opt, const void* val, socklen_t len);

int __wrap_setsockopt(int fd, int level, int opt, const void* val, socklen_t len) {
    /* 65535/4608 è SOL_SOCKET e SO_NBIO (0x1200), non SO_REUSEPORT.
     * Su un fd che non è un socket il kernel risponde ENOTSOCK (38).
     * Se il socket rifiuta 0x1200, si riprova l'ioctl FIONBIO. */
    if (level == 0xffff && opt == 0x1200) {
        int rc = __real_setsockopt(fd, level, opt, val, len);
        if (rc == 0) return 0;
        int on = (val && len >= (socklen_t)sizeof(int) && *(const int*)val) ? 1 : 0;
        if (__real_ioctl(fd, 0x8004667eUL, &on) == 0) return 0;
        xc_fold_errno();
        static int once;
        if (!once++) xc_log_flush_line("W", "setsockopt nbio rifiutato");
        return -1;
    }
    /* Linux: SO_SNDBUF=7 SO_RCVBUF=8. Orbis/FreeBSD: 0x1001 / 0x1002. */
    if (level == 0xffff || level == SOL_SOCKET) {
        if (opt == 8) opt = 0x1002;
        else if (opt == 7) opt = 0x1001;
        if (opt == 0x1002 || opt == 0x1001) {
            int want = (val && len >= (socklen_t)sizeof(int)) ? *(const int*)val : 0;
            const int sizes[] = {want, 256 * 1024, 128 * 1024, 64 * 1024, 32 * 1024};
            for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
                int sz = sizes[i];
                if (sz <= 0) continue;
                int rc = __real_setsockopt(fd, 0xffff, opt, &sz, sizeof sz);
                if (rc == 0) {
                    static int once;
                    if (!once++) {
                        char msg[48];
                        snprintf(msg, sizeof msg, "setsockopt 0x%x = %d", opt, sz);
                        xc_log_flush_line("I", msg);
                    }
                    return 0;
                }
            }
            xc_fold_errno();
            static int n;
            if (n < 4) {
                char msg[48];
                snprintf(msg, sizeof msg, "setsockopt 0x%x e%d", opt, errno);
                xc_log_flush_line("W", msg);
                ++n;
            }
            return -1;
        }
    }
    int rc = __real_setsockopt(fd, level, opt, val, len);
    /* Orbis rifiuta IP_TOS con EINVAL. Il DSCP non serve: niente avviso. */
    if (rc < 0 && level == IPPROTO_IP && opt == IP_TOS) return 0;
    /* SO_NBIO è 0x1200 e torna spesso. Si annotano solo i fallimenti, pochi. */
    if (rc < 0) {
        xc_fold_errno();
        static int n;
        if (n < 12) {
            char msg[40];
            snprintf(msg, sizeof msg, "setsockopt %d/%d e%d", level, opt, errno);
            xc_log_flush_line("W", msg);
            ++n;
        }
    }
    return rc;
}

int __wrap_pthread_setschedparam(pthread_t t, int policy, const struct sched_param* p) {
    (void)t;
    (void)policy;
    (void)p;
    static int once;
    if (!once++) xc_log_flush_line("I", "setsched");
    return 0;
}

int __wrap_pthread_getschedparam(pthread_t t, int* policy, struct sched_param* p) {
    (void)t;
    if (policy) *policy = 0;
    if (p) p->sched_priority = 0;
    static int once;
    if (!once++) xc_log_flush_line("I", "getsched");
    return 0;
}

int __wrap_pthread_attr_setschedparam(pthread_attr_t* a, const struct sched_param* p) {
    (void)a;
    (void)p;
    static int once;
    if (!once++) xc_log_flush_line("I", "attr sched");
    return 0;
}

int __wrap_pthread_attr_setschedpolicy(pthread_attr_t* a, int policy) {
    (void)a;
    (void)policy;
    return 0;
}

int __wrap_sched_setscheduler(pid_t pid, int policy, const struct sched_param* p) {
    (void)pid;
    (void)policy;
    (void)p;
    static int once;
    if (!once++) xc_log_flush_line("I", "sched_setscheduler");
    return 0;
}

int __wrap_sem_wait(sem_t* sem) {
    int rc = __real_sem_wait(sem);
    if (rc == 0) return 0;
    int e = rc < 0 ? xc_fold_sce(errno) : xc_fold_sce(rc);
    errno = e > 0 ? e : EINVAL;
    return -1;
}

int __wrap_sem_timedwait(sem_t* sem, const struct timespec* abs) {
    int rc = __real_sem_timedwait(sem, abs);
    if (rc == 0) return 0;
    int e = rc < 0 ? xc_fold_sce(errno) : xc_fold_sce(rc);
    if (xc_is_timeout(e) || e == EINTR) e = XC_LIBCXX_ETIMEDOUT;
    errno = e > 0 ? e : EINVAL;
    return -1;
}

/* La 01.18, se nanosleep falliva, chiamava sceKernelUsleep. Quel simbolo
 * rientra in questo wrap: il thread del timer SCTP non torna e il log si
 * ferma su "dtls start". La 01.19 usava la syscall 240. Il sonno di riserva
 * è sceKernelNanosleep, che prende lo stesso timespec relativo (secondi e
 * nanosecondi) e non passa da questo wrap. Se dorme, si torna 0: usrsctp
 * esce dal do/while e chiama sctp_handle_tick. */
static int xc_sys_nanosleep(const struct timespec* req, struct timespec* rem) {
    int32_t rc = sceKernelNanosleep((const OrbisKernelTimespec*)req, (OrbisKernelTimespec*)rem);
    if (rc == 0) return 0;
    unsigned u = (unsigned)rc;
    if ((u & 0xFFFF0000u) == 0x80020000u) errno = (int)(u & 0xFFFFu);
    else if (rc > 0 && rc < 4096) errno = rc;
    else errno = EIO;
    return -1;
}

void xc_raw_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)xc_sys_nanosleep(&ts, NULL);
}

int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
    static __thread int depth;
    if (depth > 0) return xc_sys_nanosleep(req, rem);
    ++depth;
    int rc = __real_nanosleep(req, rem);
    if (rc == 0) {
        --depth;
        return 0;
    }
    static int once;
    if (!once++) {
        char msg[40];
        snprintf(msg, sizeof msg, "nanosleep: real %d", rc);
        xc_log_flush_line("W", msg);
    }
    struct timespec fallback;
    const struct timespec* use = req;
    if (!use) {
        fallback.tv_sec = 0;
        fallback.tv_nsec = 10000000L;
        use = &fallback;
    }
    int slept = xc_sys_nanosleep(use, NULL);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    --depth;
    /* Sempre 0: usrsctp fa do/while su -1 e con rem=0 gira a vuoto
     * (timer SCTP bloccato → DTLS/ICE fermi). */
    (void)slept;
    return 0;
}

ssize_t __wrap_read(int fd, void* buf, size_t n) {
    if (xc_rand_has(fd)) {
        if (!buf || n == 0) return 0;
        if (!ps4_random_bytes(buf, n)) {
            errno = EIO;
            return -1;
        }
        return (ssize_t)n;
    }
    return xc_sys_sz(__real_read(fd, buf, n));
}

ssize_t __wrap_write(int fd, const void* buf, size_t n) { return xc_sys_sz(__real_write(fd, buf, n)); }

/* Orbis rifiuta sendto/bind/connect con sa_len a 0 (EINVAL 22). STUN di juice
 * scrive sin_family e non sin_len. Un IPv6 vero su un socket IPv4 è lo stesso
 * errno: lo scartiamo qui, un v4-mapped lo traduciamo. */
static int xc_sa_family(const struct sockaddr* sa, socklen_t len) {
    if (!sa || len < 2) return AF_UNSPEC;
    const unsigned char* b = (const unsigned char*)sa;
    if (b[1] == AF_INET || b[1] == AF_INET6) return b[1];
    return AF_UNSPEC;
}

static void xc_sa_set_len(struct sockaddr* sa, socklen_t len) {
    if (!sa || len < 2 || sa->sa_len != 0) return;
    int fam = xc_sa_family(sa, len);
    if (fam == AF_INET) sa->sa_len = (unsigned char)sizeof(struct sockaddr_in);
    else if (fam == AF_INET6) sa->sa_len = (unsigned char)sizeof(struct sockaddr_in6);
}

static void xc_log_send(const struct sockaddr* sa, socklen_t len, int err) {
    static int n;
    unsigned fam = 0, salen = 0, port = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    char msg[80];
    if (n >= 6) return;
    ++n;
    if (sa && len >= 2) {
        const unsigned char* b = (const unsigned char*)sa;
        salen = b[0];
        fam = b[1];
        if (fam == AF_INET && len >= sizeof(struct sockaddr_in)) {
            const struct sockaddr_in* s4 = (const struct sockaddr_in*)sa;
            const unsigned char* a = (const unsigned char*)&s4->sin_addr;
            port = ntohs(s4->sin_port);
            a0 = a[0];
            a1 = a[1];
            a2 = a[2];
            a3 = a[3];
        } else if (fam == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
            port = ntohs(((const struct sockaddr_in6*)sa)->sin6_port);
        }
    }
    snprintf(msg, sizeof msg, "send: fam %u len %u sa %u %u.%u.%u.%u:%u e %d", fam, (unsigned)len, salen, a0, a1,
             a2, a3, port, err);
    xc_log_flush_line("W", msg);
}

static int xc_copy_sa(struct sockaddr_storage* ss, const struct sockaddr* addr, socklen_t len, socklen_t* outlen) {
    socklen_t copy;
    memset(ss, 0, sizeof *ss);
    if (!addr || len < 2) return -1;
    copy = len < sizeof *ss ? len : (socklen_t)sizeof *ss;
    memcpy(ss, addr, copy);
    xc_sa_set_len((struct sockaddr*)ss, copy);
    *outlen = ((struct sockaddr*)ss)->sa_len ? ((struct sockaddr*)ss)->sa_len : copy;
    return 0;
}

ssize_t __real_recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* addr, socklen_t* len);
ssize_t __wrap_recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* addr, socklen_t* len) {
    ssize_t rc = xc_sys_sz(__real_recvfrom(fd, buf, n, flags, addr, len));
    if (rc >= 0 && addr && len && *len >= 2) xc_sa_set_len(addr, *len);
    return rc;
}

static void xc_log_send_ok(const struct sockaddr_in* s4) {
    static int once;
    char msg[64];
    const unsigned char* a;
    if (once) return;
    once = 1;
    a = (const unsigned char*)&s4->sin_addr;
    snprintf(msg, sizeof msg, "send: ok fam 2 %u.%u.%u.%u:%u", a[0], a[1], a[2], a[3],
             (unsigned)ntohs(s4->sin_port));
    xc_log_flush_line("I", msg);
}

/* usrsctp manda sendmsg con un iovec per mbuf. Il primo è spesso 24 byte
 * e il kernel, per UDP (protocollo 17), deve fare m_pullup fino a 30:
 * il klog si riempie di "delayed m_pullup". Un solo buffer contiguo no.
 * Stessa cosa in ricezione: recvmsg con una catena di iovec. */
static uint8_t* xc_gather(const struct iovec* iov, int n, size_t* out) {
    size_t total = 0;
    int i;
    for (i = 0; i < n; i++) total += iov[i].iov_len;
    if (total > 65536) return NULL;
    uint8_t* buf = malloc(total ? total : 1);
    if (!buf) return NULL;
    size_t off = 0;
    for (i = 0; i < n; i++) {
        if (!iov[i].iov_len) continue;
        memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
        off += iov[i].iov_len;
    }
    *out = total;
    return buf;
}

ssize_t __wrap_sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len);
ssize_t __real_sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t __wrap_sendmsg(int fd, const struct msghdr* msg, int flags) {
    int fl = flags;
#ifdef MSG_NOSIGNAL
    fl &= ~MSG_NOSIGNAL;
#endif
    if (!msg || msg->msg_iovlen <= 1) return __real_sendmsg(fd, msg, fl);
    size_t total = 0;
    uint8_t* buf = xc_gather(msg->msg_iov, (int)msg->msg_iovlen, &total);
    if (!buf) {
        errno = EMSGSIZE;
        return -1;
    }
    ssize_t rc = __wrap_sendto(fd, buf, total, fl, (const struct sockaddr*)msg->msg_name, msg->msg_namelen);
    free(buf);
    return rc;
}

ssize_t __real_recvmsg(int fd, struct msghdr* msg, int flags);
ssize_t __wrap_recvmsg(int fd, struct msghdr* msg, int flags) {
    if (!msg || msg->msg_iovlen <= 1) return __real_recvmsg(fd, msg, flags);
    size_t total = 0;
    int i;
    for (i = 0; i < (int)msg->msg_iovlen; i++) total += msg->msg_iov[i].iov_len;
    if (total > 65536) total = 65536;
    uint8_t* buf = malloc(total ? total : 1);
    if (!buf) {
        errno = ENOMEM;
        return -1;
    }
    struct iovec one;
    one.iov_base = buf;
    one.iov_len = total;
    struct msghdr tmp = *msg;
    tmp.msg_iov = &one;
    tmp.msg_iovlen = 1;
    ssize_t n = __real_recvmsg(fd, &tmp, flags);
    if (n > 0) {
        size_t left = (size_t)n;
        uint8_t* p = buf;
        for (i = 0; i < (int)msg->msg_iovlen && left; i++) {
            size_t c = msg->msg_iov[i].iov_len;
            if (c > left) c = left;
            if (c && msg->msg_iov[i].iov_base) memcpy(msg->msg_iov[i].iov_base, p, c);
            p += c;
            left -= c;
        }
    }
    msg->msg_flags = tmp.msg_flags;
    msg->msg_namelen = tmp.msg_namelen;
    msg->msg_controllen = tmp.msg_controllen;
    free(buf);
    return n;
}

ssize_t __real_sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len);
ssize_t __wrap_sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len) {
    int fl = flags;
    ssize_t rc;
    xc_sockaddr_in v4;
#ifdef MSG_NOSIGNAL
    fl &= ~MSG_NOSIGNAL;
#endif
    /* Qualunque ::ffff:a.b.c.d (o un IPv4 lasciato all'offset sbagliato
     * dentro fam 28) esce come sockaddr_in: sin_len 16, famiglia 2. */
    if (addr && xc_sa_to_inet4(addr, len, &v4)) {
        struct sockaddr_in s4;
        memset(&s4, 0, sizeof s4);
        s4.sin_len = 16;
        s4.sin_family = AF_INET;
        s4.sin_port = v4.sin_port;
        memcpy(&s4.sin_addr, &v4.sin_addr, 4);
        rc = xc_sys_sz(__real_sendto(fd, buf, n, fl, (struct sockaddr*)&s4, sizeof s4));
        if (rc >= 0) xc_log_send_ok(&s4);
        else xc_log_send((struct sockaddr*)&s4, sizeof s4, errno);
        return rc;
    }
    if (addr && len >= 2 && xc_sa_family(addr, len) == AF_INET6) {
        static int once;
        if (!once++) xc_log_flush_line("W", "send: ipv6 su socket ipv4");
        errno = EAFNOSUPPORT;
        xc_log_send(addr, len, errno);
        return -1;
    }
    rc = xc_sys_sz(__real_sendto(fd, buf, n, fl, addr, len));
    if (rc < 0) xc_log_send(addr, len, errno);
    return rc;
}

static int xc_prep_sa(struct sockaddr_in* s4, struct sockaddr_storage* ss, const struct sockaddr* addr, socklen_t len,
                      const struct sockaddr** out, socklen_t* outlen) {
    xc_sockaddr_in v4;
    if (addr && xc_sa_to_inet4(addr, len, &v4)) {
        memset(s4, 0, sizeof *s4);
        s4->sin_len = 16;
        s4->sin_family = AF_INET;
        s4->sin_port = v4.sin_port;
        memcpy(&s4->sin_addr, &v4.sin_addr, 4);
        *out = (const struct sockaddr*)s4;
        *outlen = sizeof *s4;
        return 0;
    }
    if (xc_copy_sa(ss, addr, len, outlen) != 0) return -1;
    *out = (const struct sockaddr*)ss;
    return 0;
}

int __real_bind(int fd, const struct sockaddr* addr, socklen_t len);
int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t len) {
    struct sockaddr_in s4;
    struct sockaddr_storage ss;
    const struct sockaddr* out = addr;
    socklen_t outlen = len;
    if (xc_prep_sa(&s4, &ss, addr, len, &out, &outlen) != 0) return xc_sys_int(__real_bind(fd, addr, len));
    return xc_sys_int(__real_bind(fd, out, outlen));
}

int __real_connect(int fd, const struct sockaddr* addr, socklen_t len);
int __wrap_connect(int fd, const struct sockaddr* addr, socklen_t len) {
    struct sockaddr_in s4;
    struct sockaddr_storage ss;
    const struct sockaddr* out = addr;
    socklen_t outlen = len;
    if (xc_prep_sa(&s4, &ss, addr, len, &out, &outlen) != 0) return xc_sys_int(__real_connect(fd, addr, len));
    return xc_sys_int(__real_connect(fd, out, outlen));
}

/* usrsctp: il costruttore di SctpTransport abortisce al primo setsockopt.
 * Le opzioni che Orbis/usrsctp rifiuta (EINVAL, ENOPROTOOPT, ...) non devono
 * uccidere l'associazione. socket, bind e connect restano errori veri. */
static int xc_sctp_opt_skip(int e) {
    if (e == EINVAL || e == ENOPROTOOPT || e == EOPNOTSUPP || e == EAFNOSUPPORT || e == ENOTTY) return 1;
#ifdef ENOTSUP
    if (e == ENOTSUP) return 1;
#endif
    return 0;
}

void* __real_usrsctp_socket(int domain, int type, int protocol, void* rcb, void* scb, uint32_t thr, void* ulp);
void* __wrap_usrsctp_socket(int domain, int type, int protocol, void* rcb, void* scb, uint32_t thr, void* ulp) {
    void* so = __real_usrsctp_socket(domain, type, protocol, rcb, scb, thr, ulp);
    char msg[48];
    if (so) {
        xc_log_flush_line("I", "sctp: socket ok");
        return so;
    }
    snprintf(msg, sizeof msg, "sctp: socket errno %d", errno);
    xc_log_flush_line("E", msg);
    return NULL;
}

/* usrsctp vuole len=16 al byte 0 e AF_CONN al byte 1. Se libdatachannel ha
 * scritto la famiglia come uint16, la si raddrizza prima della chiamata. */
static void xc_sctp_prepare(struct sockaddr* name, int namelen, const char* what) {
    if (!name || namelen < 4) return;
    if (xc_sconn_fix((unsigned char*)name, namelen)) {
        char msg[48];
        snprintf(msg, sizeof msg, "sctp: %s allineo", what);
        xc_log_flush_line("I", msg);
    }
}

static void xc_sctp_log(const char* what, int rc, int e, const struct sockaddr* name) {
    char msg[72];
    unsigned lenb = 0, fam = 0;
    if (name) {
        lenb = ((const unsigned char*)name)[0];
        fam = ((const unsigned char*)name)[1];
    }
    if (rc == 0) snprintf(msg, sizeof msg, "sctp: %s ok len %u fam %u", what, lenb, fam);
    else snprintf(msg, sizeof msg, "sctp: %s errno %d len %u fam %u", what, e, lenb, fam);
    xc_log_flush_line(rc == 0 || e == EINPROGRESS ? "I" : "E", msg);
}

int __real_usrsctp_bind(void* so, struct sockaddr* name, int namelen);
int __wrap_usrsctp_bind(void* so, struct sockaddr* name, int namelen) {
    int e;
    xc_sctp_prepare(name, namelen, "bind");
    int rc = __real_usrsctp_bind(so, name, namelen);
    e = errno;
    xc_sctp_log("bind", rc, e, name);
    errno = e;
    return rc;
}

int __real_usrsctp_connect(void* so, struct sockaddr* name, int namelen);
int __wrap_usrsctp_connect(void* so, struct sockaddr* name, int namelen) {
    int e;
    xc_sctp_prepare(name, namelen, "connect");
    int rc = __real_usrsctp_connect(so, name, namelen);
    e = errno;
    xc_sctp_log("connect", rc, e, name);
    errno = e;
    return rc;
}

int __real_usrsctp_setsockopt(void* so, int level, int opt, const void* val, socklen_t len);
int __wrap_usrsctp_setsockopt(void* so, int level, int opt, const void* val, socklen_t len) {
    int rc = __real_usrsctp_setsockopt(so, level, opt, val, len);
    int e;
    char msg[64];
    if (rc == 0) return 0;
    e = errno;
    snprintf(msg, sizeof msg, "sctp: setsockopt %d %d errno %d", level, opt, e);
    xc_log_flush_line("W", msg);
    if (xc_sctp_opt_skip(e)) return 0;
    return rc;
}

int __real_usrsctp_getsockopt(void* so, int level, int opt, void* val, socklen_t* len);
int __wrap_usrsctp_getsockopt(void* so, int level, int opt, void* val, socklen_t* len) {
    int rc = __real_usrsctp_getsockopt(so, level, opt, val, len);
    int e;
    char msg[64];
    if (rc == 0) return 0;
    e = errno;
    snprintf(msg, sizeof msg, "sctp: getsockopt %d %d errno %d", level, opt, e);
    xc_log_flush_line("W", msg);
    if (xc_sctp_opt_skip(e)) return 0;
    return rc;
}

int __wrap_poll(struct pollfd* fds, nfds_t n, int timeout) {
    return xc_sys_int(__real_poll(fds, n, timeout));
}

int __wrap_pthread_rwlock_timedrdlock(pthread_rwlock_t* lock, const struct timespec* abs) {
    return __wrap_pthread_mutex_timedlock(rw_as_mutex(lock), abs);
}

int __wrap_pthread_rwlock_timedwrlock(pthread_rwlock_t* lock, const struct timespec* abs) {
    return __wrap_pthread_mutex_timedlock(rw_as_mutex(lock), abs);
}

/* abort di musl non scrive nulla: un throw senza unwinder, o std::terminate,
 * muore qui. Il timbro è write+fsync, poi il vero abort. */
extern void xc_log_stamp(const char* msg);
extern void __real_abort(void);
void __wrap_abort(void) {
    xc_log_stamp("abort");
    __real_abort();
}

/* exit() di libc fa girare atexit e i distruttori statici (libdatachannel)
 * con i thread ancora vivi: CE-34878-0 in chiusura. Solo _exit, mai i dtor. */
static void (*xc_exit_fn)(void);
void xc_set_exit_hook(void (*fn)(void)) { xc_exit_fn = fn; }

void __wrap_exit(int status) {
    xc_log_stamp("exit: exit()");
    if (xc_exit_fn) xc_exit_fn(); /* tipicamente stamp+_exit, non ritorna */
    _exit(status);
}
