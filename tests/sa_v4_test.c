/* Round-trip offline degli indirizzi Orbis. gcc -std=c11 tests/sa_v4_test.c */
#include "../src/platform/ps4/sa_v4.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(const char* name, int cond) {
    if (cond) return;
    fprintf(stderr, "FAIL %s\n", name);
    g_fail++;
}

static void fill_v4(unsigned char* b, unsigned port, unsigned a, unsigned c, unsigned d, unsigned e) {
    memset(b, 0, 16);
    b[0] = 16;
    b[1] = XC_AF_INET;
    b[2] = (unsigned char)(port >> 8);
    b[3] = (unsigned char)port;
    b[4] = (unsigned char)a;
    b[5] = (unsigned char)c;
    b[6] = (unsigned char)d;
    b[7] = (unsigned char)e;
}

static void fill_mapped(unsigned char* b, unsigned port, unsigned a, unsigned c, unsigned d, unsigned e) {
    memset(b, 0, 28);
    b[0] = 28;
    b[1] = XC_AF_INET6;
    b[2] = (unsigned char)(port >> 8);
    b[3] = (unsigned char)port;
    b[18] = 0xff;
    b[19] = 0xff;
    b[20] = (unsigned char)a;
    b[21] = (unsigned char)c;
    b[22] = (unsigned char)d;
    b[23] = (unsigned char)e;
}

static void check_v4(const char* name, const xc_sockaddr_in* s, unsigned port, unsigned a, unsigned c, unsigned d,
                     unsigned e) {
    const unsigned char* ip = (const unsigned char*)&s->sin_addr;
    expect(name, s->sin_len == 16);
    expect(name, s->sin_family == XC_AF_INET);
    expect(name, s->sin_port == (uint16_t)((port >> 8) | ((port & 0xff) << 8)));
    expect(name, ip[0] == a && ip[1] == c && ip[2] == d && ip[3] == e);
}

int main(void) {
    unsigned char raw[28];
    xc_sockaddr_in out;

    fill_v4(raw, 3478, 192, 168, 1, 19);
    expect("v4", xc_sa_to_inet4(raw, 16, &out) == 1);
    check_v4("v4", &out, 3478, 192, 168, 1, 19);

    fill_mapped(raw, 3478, 192, 168, 1, 19);
    expect("mapped", xc_sa_to_inet4(raw, 28, &out) == 1);
    check_v4("mapped", &out, 3478, 192, 168, 1, 19);

    /* Famiglia 28, IPv4 lasciato a offset 4, sin6_addr a zero. */
    memset(raw, 0, sizeof raw);
    raw[0] = 28;
    raw[1] = XC_AF_INET6;
    raw[2] = 0x0d;
    raw[3] = 0x96;
    raw[4] = 192;
    raw[5] = 168;
    raw[6] = 1;
    raw[7] = 19;
    expect("offset", xc_sa_to_inet4(raw, 28, &out) == 1);
    check_v4("offset", &out, 3478, 192, 168, 1, 19);

    /* IPv6 vero: non diventa 0.0.0.0. */
    memset(raw, 0, sizeof raw);
    raw[0] = 28;
    raw[1] = XC_AF_INET6;
    raw[2] = 0x04;
    raw[3] = 0x40;
    raw[8] = 0x20;
    raw[9] = 0x01;
    raw[23] = 1;
    expect("v6", xc_sa_to_inet4(raw, 28, &out) == 0);

    /* Linux: famiglia uint16 al byte 0, mapped a offset 20. */
    memset(raw, 0, sizeof raw);
    raw[0] = XC_AF_INET6;
    raw[1] = 0;
    raw[2] = 0x0d;
    raw[3] = 0x96;
    raw[18] = 0xff;
    raw[19] = 0xff;
    raw[20] = 10;
    raw[21] = 0;
    raw[22] = 0;
    raw[23] = 1;
    expect("linux", xc_sa_to_inet4(raw, 28, &out) == 1);
    check_v4("linux", &out, 3478, 10, 0, 0, 1);

    if (g_fail) {
        fprintf(stderr, "%d controlli falliti\n", g_fail);
        return 1;
    }
    printf("sa_v4: ok\n");
    return 0;
}
