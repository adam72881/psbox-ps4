/* SPDX-License-Identifier: GPL-3.0-or-later
 * Indirizzi BSD di Orbis. sockaddr_in6 è 28 byte: famiglia al byte 1,
 * sin6_addr a offset 8, l'IPv4 di ::ffff:a.b.c.d a offset 20.
 * Se la famiglia è stata messa a 28 senza spostare l'IPv4, l'indirizzo
 * resta a offset 4 (dove stava sin_addr) e sin6_addr è zero. */
#pragma once

#include <stdint.h>
#include <string.h>

#define XC_AF_INET 2
#define XC_AF_INET6 28

typedef struct xc_sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
} xc_sockaddr_in;

/* 1 se dst è un sockaddr_in da 16 byte (sin_len 16, famiglia 2). */
static inline int xc_sa_to_inet4(const void* src, unsigned len, xc_sockaddr_in* dst) {
    const unsigned char* b = (const unsigned char*)src;
    int i;
    if (!src || !dst || len < 8) return 0;
    memset(dst, 0, sizeof *dst);
    /* Linux: famiglia uint16 al byte 0. AF_INET6 = 28 → 1c 00. */
    if (b[1] == XC_AF_INET && !(b[0] == XC_AF_INET6 && b[1] == 0)) {
        dst->sin_len = 16;
        dst->sin_family = XC_AF_INET;
        dst->sin_port = (uint16_t)(b[2] | ((uint16_t)b[3] << 8));
        memcpy(&dst->sin_addr, b + 4, 4);
        return 1;
    }
    if (b[1] != XC_AF_INET6 && !(b[0] == XC_AF_INET6 && b[1] == 0)) return 0;
    if (len >= 24) {
        int mapped = 1;
        for (i = 8; i < 18; i++)
            if (b[i] != 0) mapped = 0;
        if (mapped && b[18] == 0xff && b[19] == 0xff) {
            dst->sin_len = 16;
            dst->sin_family = XC_AF_INET;
            dst->sin_port = (uint16_t)(b[2] | ((uint16_t)b[3] << 8));
            memcpy(&dst->sin_addr, b + 20, 4);
            return 1;
        }
        for (i = 8; i < 24; i++)
            if (b[i] != 0) return 0; /* IPv6 vero */
    }
    /* Famiglia 28 ma l'IPv4 è rimasto dov'era sin_addr. */
    if (b[4] || b[5] || b[6] || b[7]) {
        dst->sin_len = 16;
        dst->sin_family = XC_AF_INET;
        dst->sin_port = (uint16_t)(b[2] | ((uint16_t)b[3] << 8));
        memcpy(&dst->sin_addr, b + 4, 4);
        return 1;
    }
    return 0;
}
