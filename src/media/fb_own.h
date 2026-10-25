/* SPDX-License-Identifier: GPL-3.0-or-later
 * Un framebuffer accettato da Videodec2 non si offre di nuovo finché
 * l'immagine non è stata copiata. La 01.49 avanzava l'indice solo quando
 * usciva una picture: senza picture lo stesso buffer veniva riofferto
 * mentre il decoder poteva ancora tenerlo.
 */
#ifndef PSBOX_FB_OWN_H
#define PSBOX_FB_OWN_H

#include <stddef.h>

struct FbOwn {
    int n;
    int held[8];
};

static void fbOwnInit(FbOwn* o, int n) {
    int i;
    if (!o) return;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    o->n = n;
    for (i = 0; i < 8; ++i) o->held[i] = 0;
}

static int fbOffer(const FbOwn* o) {
    int i;
    if (!o) return -1;
    for (i = 0; i < o->n; ++i)
        if (!o->held[i]) return i;
    return -1;
}

static void fbOnAccept(FbOwn* o, int slot, int accepted) {
    if (!o || slot < 0 || slot >= o->n) return;
    o->held[slot] = accepted ? 1 : 0;
}

static void fbRelease(FbOwn* o, int slot) {
    if (!o || slot < 0 || slot >= o->n) return;
    o->held[slot] = 0;
}

static int fbHeldCount(const FbOwn* o) {
    int i, n = 0;
    if (!o) return 0;
    for (i = 0; i < o->n; ++i) n += o->held[i] ? 1 : 0;
    return n;
}

static int fbIndexOf(const void* base, size_t stride, const void* ptr, int n) {
    const unsigned char* b;
    const unsigned char* p;
    size_t off;
    int i;
    if (!base || !ptr || !stride || n <= 0) return -1;
    b = (const unsigned char*)base;
    p = (const unsigned char*)ptr;
    if (p < b) return -1;
    off = (size_t)(p - b);
    if (off % stride) return -1;
    i = (int)(off / stride);
    if (i < 0 || i >= n) return -1;
    return i;
}

#endif
