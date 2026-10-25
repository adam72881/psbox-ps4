// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int ps4_random_bytes(void* out, size_t len);
void xc_dtls_note_rx(size_t n);
unsigned xc_dtls_rx_pkts(void);
unsigned xc_dtls_rx_bytes(void);
void xc_dtls_rx_reset(void);

#ifdef __cplusplus
}
#endif
