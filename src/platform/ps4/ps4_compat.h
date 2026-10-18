// SPDX-License-Identifier: GPL-3.0-or-later
// Forzato in ogni unità di traduzione, come ps5/compat/ps5_lfs.h.
// OpenOrbis non dichiara FIONBIO: libjuice (WebRTC/ICE) lo passa a ioctl.
// Il valore è quello FreeBSD, _IOW('f', 126, int). libc_compat.c lo traduce
// in setsockopt(SO_NBIO), perché la sandbox rifiuta FIONBIO sui socket.
#pragma once
#include <stddef.h>
#ifndef FIONBIO
#define FIONBIO 0x8004667eUL
#endif
/* FreeBSD sys/param.h. usrsctp ridefinisce CMSG_ALIGN come _ALIGN, che musl non ha. */
#ifndef _ALIGN
#define _ALIGNBYTES (sizeof(long) - 1)
#define _ALIGN(p) (((unsigned long)(p) + _ALIGNBYTES) & ~_ALIGNBYTES)
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* OpenOrbis non li dichiara. Le definizioni sono in libc_compat.c. */
int timingsafe_bcmp(const void* a, const void* b, size_t n);
void arc4random_buf(void* buf, size_t n);
#ifdef __cplusplus
}
#endif
