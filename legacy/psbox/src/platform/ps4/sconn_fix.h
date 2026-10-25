/* SPDX-License-Identifier: GPL-3.0-or-later
 * sockaddr_conn di usrsctp su Orbis (HAVE_SCONN_LEN, come FreeBSD):
 *   byte 0  sconn_len     = 16
 *   byte 1  sconn_family  = AF_CONN (123)
 *   byte 2  sconn_port    (uint16, network order)
 *   byte 8  sconn_addr
 * Il layout Linux mette la famiglia in un uint16 all'offset 0: byte 0 = 123,
 * byte 1 = 0. Porta e puntatore restano agli stessi offset. usrsctp legge la
 * famiglia al byte 1 e rifiuta con EAFNOSUPPORT (47) se è 0; se la famiglia
 * è giusta ma la lunghezza non è 16, sctpconn_bind torna EINVAL (22). */
#pragma once

#define XC_AF_CONN 123
#define XC_SCONN_SIZE 16

/* Ritorna 1 se ha convertito il layout Linux. Non tocca AF_INET e il resto. */
static inline int xc_sconn_fix(unsigned char* p, int len) {
    if (!p || len < 4) return 0;
    if (p[1] == XC_AF_CONN) {
        p[0] = XC_SCONN_SIZE;
        return 0;
    }
    if (p[0] == XC_AF_CONN && p[1] == 0) {
        p[0] = XC_SCONN_SIZE;
        p[1] = XC_AF_CONN;
        return 1;
    }
    return 0;
}
