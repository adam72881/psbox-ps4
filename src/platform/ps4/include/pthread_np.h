/* Shim: usrsctp include <pthread_np.h> su FreeBSD.
 * OpenOrbis dichiara pthread_set_name_np in pthread.h. */
#ifndef _PTHREAD_NP_H_
#define _PTHREAD_NP_H_
#include <pthread.h>
#endif
