/* Shim: usrsctp (percorso FreeBSD userspace) include <sys/mutex.h>.
 * OpenOrbis non lo ha. I lock veri sono no-op già in sctp_os_userspace.h
 * (SCTP_PROCESS_LEVEL_LOCKS). */
#ifndef _SYS_MUTEX_H_
#define _SYS_MUTEX_H_
struct mtx {
    int dummy;
};
#endif
