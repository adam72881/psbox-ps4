// SPDX-License-Identifier: GPL-3.0-or-later
// Entropia per mbedTLS e per platform::randomBytes sulla PS4.
// Preferenza: sceKernelGetRandomNumber / sceNetGetRandom (dlsym). Ripiego:
// TSC + orologio + sandbox (come Moonlight-ps4). Non si apre /dev/urandom.
#include <orbis/libkernel.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int (*XcGetRandomFn)(void* buf, size_t n);

static XcGetRandomFn g_kernelRandom;
static XcGetRandomFn g_netRandom;
static int g_randomTried;

static void xc_find_random(void) {
    if (g_randomTried) return;
    g_randomTried = 1;
    OrbisKernelModule list[128];
    memset(list, 0, sizeof list);
    size_t avail = 0;
    if (sceKernelGetModuleList(list, 128, &avail) != 0) return;
    size_t n = avail > 128 ? 128 : avail;
    const char* names[] = {"sceKernelGetRandomNumber", "sceNetGetRandom", "sceRandomGetRandomNumber"};
    for (size_t i = 0; i < n; ++i) {
        if ((int)list[i] <= 0) continue;
        for (unsigned k = 0; k < sizeof names / sizeof names[0]; ++k) {
            void* sym = NULL;
            if (sceKernelDlsym((int)list[i], names[k], &sym) != 0 || !sym) continue;
            if (k == 0 && !g_kernelRandom) g_kernelRandom = (XcGetRandomFn)sym;
            else if (k == 1 && !g_netRandom) g_netRandom = (XcGetRandomFn)sym;
            else if (k == 2 && !g_kernelRandom) g_kernelRandom = (XcGetRandomFn)sym;
        }
    }
}

static uint64_t mix64(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void mix_u64(uint64_t* state, uint64_t v) {
    *state ^= v;
    (void)mix64(state);
}

static int xc_fallback_poll(unsigned char* output, size_t len, size_t* olen) {
    uint64_t state = sceKernelReadTsc();
    mix_u64(&state, sceKernelGetProcessTime());
    mix_u64(&state, sceKernelGetProcessTimeCounter());
    mix_u64(&state, (uint64_t)(uintptr_t)&state);
    const char* sandbox = sceKernelGetFsSandboxRandomWord();
    if (sandbox) {
        for (const char* p = sandbox; *p; ++p) mix_u64(&state, (unsigned char)*p);
    }
    for (int i = 0; i < 32; ++i) {
        uint64_t a = sceKernelReadTsc();
        uint64_t b = sceKernelReadTsc();
        mix_u64(&state, a ^ (b << 1) ^ (uint64_t)i);
    }

    size_t off = 0;
    while (off < len) {
        uint64_t r = mix64(&state);
        size_t n = len - off;
        if (n > sizeof r) n = sizeof r;
        memcpy(output + off, &r, n);
        off += n;
    }
    *olen = len;
    return 0;
}

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    if (!output || !olen || len == 0) return -1;

    xc_find_random();
    if (g_kernelRandom && g_kernelRandom(output, len) == 0) {
        *olen = len;
        return 0;
    }
    if (g_netRandom && g_netRandom(output, len) == 0) {
        *olen = len;
        return 0;
    }
    return xc_fallback_poll(output, len, olen);
}

int ps4_random_bytes(void* out, size_t len) {
    size_t got = 0;
    return mbedtls_hardware_poll(NULL, (unsigned char*)out, len, &got) == 0 && got == len;
}

// Contatore DTLS in ingresso (byte 20-63 su ICE). Usato per ripartire se
// dopo ICE connected non arriva nulla entro pochi secondi.
static volatile unsigned g_dtls_rx_pkts;
static volatile unsigned g_dtls_rx_bytes;

void xc_dtls_note_rx(size_t n) {
    if (n == 0) return;
    __sync_fetch_and_add(&g_dtls_rx_pkts, 1u);
    __sync_fetch_and_add(&g_dtls_rx_bytes, (unsigned)n);
}

unsigned xc_dtls_rx_pkts(void) { return g_dtls_rx_pkts; }
unsigned xc_dtls_rx_bytes(void) { return g_dtls_rx_bytes; }
void xc_dtls_rx_reset(void) {
    g_dtls_rx_pkts = 0;
    g_dtls_rx_bytes = 0;
}
