/* SPDX-License-Identifier: GPL-3.0-only */
#include "canvas.h"
#include "font.h"
#include <stdint.h>

uint32_t x4_rgb(unsigned r, unsigned g, unsigned b)
{ return 0xff000000u | ((b & 255u) << 16) | ((g & 255u) << 8) | (r & 255u); }
void x4_rect(uint32_t *p, int x, int y, int w, int h, uint32_t color)
{
    int64_t right = (int64_t)x + w, bottom = (int64_t)y + h;
    if (!p || w <= 0 || h <= 0) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > X4_WIDTH) right = X4_WIDTH;
    if (bottom > X4_HEIGHT) bottom = X4_HEIGHT;
    for (int row = y; row < bottom; ++row)
        for (int col = x; col < right; ++col) p[(size_t)row * X4_WIDTH + col] = color;
}
static int legacy_size(int scale)
{
    switch (scale) {
    case 1: return 18;
    case 2: return 26;
    case 3: return 32;
    case 4: return 38;
    case 5: return 46;
    default: return 56;
    }
}
void x4_text(uint32_t *p, int x, int y, int scale, const char *text, uint32_t color)
{ if (scale > 0) x4_text_px(p, x, y, legacy_size(scale), 0, text, color); }
void x4_text_literal(uint32_t *p, int x, int y, int scale, const char *text, uint32_t color)
{ if (scale > 0) x4_text_px(p, x, y, legacy_size(scale), 0, text, color); }
