/* SPDX-License-Identifier: GPL-3.0-or-later
 * 1920→1280 è 3:2. I pixel dest pari cadono su un sorgente intero.
 * Quelli dispari cadono a metà fra due sorgenti: si mediano, non si
 * prende solo il nearest. Due tap, stesso passaggio, niente secondo scan.
 */
#ifndef PSBOX_SCALE_3TO2_H
#define PSBOX_SCALE_3TO2_H

static void scale3to2Tap(int dst, int srcW, int* i0, int* i1) {
    int num, s;
    if (!i0 || !i1) return;
    if (srcW < 1) srcW = 1;
    if (dst < 0) dst = 0;
    num = dst * 3;
    s = num / 2;
    if (s >= srcW) s = srcW - 1;
    if (s < 0) s = 0;
    if ((num & 1) == 0 || s + 1 >= srcW) {
        *i0 = s;
        *i1 = s;
        return;
    }
    *i0 = s;
    *i1 = s + 1;
}

#endif
