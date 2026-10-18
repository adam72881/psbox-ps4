t fd, void* buf, size_t nbytes);
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
#include "platform/ps4/sconn_form.h"

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
 * scePthrea