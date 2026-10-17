/* Shim: usrsctp include <net/if_types.h> quando __FreeBSD__ è definito.
 * OpenOrbis non lo ha. IFT_LOOP lo definisce poi sctp_os_userspace.h. */
#ifndef _NET_IF_TYPES_H_
#define _NET_IF_TYPES_H_
#endif
