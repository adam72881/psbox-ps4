/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/font.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 1920
#define HEIGHT 1080
static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static uint64_t hash(const uint32_t *p)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < WIDTH * HEIGHT; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}
static void clear(uint32_t *p)
{ for (size_t i = 0; i < WIDTH * HEIGHT; ++i) p[i] = 0xff15100bu; }
static int valid_utf8(const char *p)
{
    while (*p) if (x4_utf8_next(&p) == 0xfffdu) return 0;
    return 1;
}
static void ppm(const char *path, uint32_t *p)
{
    FILE *f = fopen(path, "wb"); CHECK(f != NULL);
    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (size_t i = 0; i < WIDTH * HEIGHT; ++i) {
        unsigned char b[3] = {(unsigned char)p[i], (unsigned char)(p[i] >> 8), (unsigned char)(p[i] >> 16)};
        CHECK(fwrite(b, 1, 3, f) == 3);
    }
    CHECK(fclose(f) == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    uint32_t *allocation = malloc((WIDTH * HEIGHT + 2u) * sizeof(uint32_t));
    CHECK(allocation != NULL); uint32_t *pixels = allocation + 1;
    allocation[0] = allocation[WIDTH * HEIGHT + 1u] = 0x49afee13u;
    x4_font_close(); CHECK(x4_font_is_fallback()); CHECK(x4_font_heap_bytes() == 0);
    clear(pixels); uint64_t blank = hash(pixels);
    x4_text_px(pixels, 64, 64, 32, 0, "Espa\xc3\xb1ol: \xc3\xa1 \xc3\xa9 \xc3\xad \xc3\xb3 \xc3\xba \xc3\xbc \xc2\xbf \xc2\xa1", 0xffffffffu);
    CHECK(hash(pixels) != blank);
    CHECK(x4_font_init(argv[1]) == 0); CHECK(!x4_font_is_fallback());
    CHECK(x4_font_heap_bytes() == 3346263u);
    CHECK(x4_font_heap_bytes() + 184531u <= 4u * 1024u * 1024u);
    CHECK(x4_text_width_px(32, 0, "") == 0);
    CHECK(x4_text_width_px(32, 0, NULL) == 0);
    CHECK(x4_text_width_px(32, 0, "iii") < x4_text_width_px(32, 0, "WWW"));
    CHECK(x4_text_width_px(32, 0, "a") != x4_text_width_px(32, 0, "A"));
    const char *spanish = "\xc3\xb1\xc3\xbc\xc2\xbf\xc2\xa1\xc3\xa1\xc3\xa9\xc3\xad\xc3\xb3\xc3\xba";
    const uint32_t expected[] = {0xf1, 0xfc, 0xbf, 0xa1, 0xe1, 0xe9, 0xed, 0xf3, 0xfa};
    const char *cursor = spanish;
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) CHECK(x4_utf8_next(&cursor) == expected[i]);
    CHECK(x4_utf8_next(&cursor) == 0);
    const char *invalid[] = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0\x9f", "\xe2x", "\x80"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cursor = invalid[i]; CHECK(x4_utf8_next(&cursor) == 0xfffdu);
        while (*cursor) { const char *before = cursor; (void)x4_utf8_next(&cursor); CHECK(cursor > before); }
    }
    char fitted[128];
    CHECK(x4_text_fit_utf8(fitted, sizeof(fitted), 32, 0, spanish, 2000) == strlen(spanish));
    CHECK(!strcmp(fitted, spanish));
    CHECK(x4_text_fit_utf8(fitted, sizeof(fitted), 32, 0, "Una aventura extraordinariamente larga en M\xc3\xa9xico", 250) > 0);
    CHECK(valid_utf8(fitted)); CHECK(x4_text_width_px(32, 0, fitted) <= 250);
    CHECK(strstr(fitted, "\xe2\x80\xa6") != NULL);
    for (int width = 1; width < 700; ++width) {
        x4_text_fit_utf8(fitted, sizeof(fitted), 28, 1, spanish, width);
        CHECK(valid_utf8(fitted)); CHECK(x4_text_width_px(28, 1, fitted) <= width);
    }
    for (size_t cap = 1; cap < 32; ++cap) {
        char guarded[36]; memset(guarded, 0x55, sizeof(guarded));
        x4_text_fit_utf8(guarded + 1, cap, 32, 0, spanish, 90);
        CHECK(guarded[0] == 0x55); CHECK(guarded[cap + 1] == 0x55);
        CHECK(memchr(guarded + 1, 0, cap) != NULL); CHECK(valid_utf8(guarded + 1));
    }
    x4_text_fit_utf8(fitted, sizeof(fitted), 28, 0, "bad\xc0\xaf", 700);
    CHECK(strstr(fitted, "\xef\xbf\xbd") != NULL);
    CHECK(x4_text_fit_utf8(fitted, sizeof(fitted), 28, 0, spanish, 0) == 0);
    CHECK(x4_text_fit_utf8(NULL, 0, 28, 0, spanish, 0) == 0);
    clear(pixels); x4_text_px(pixels, 0, 0, 32, 0, "lowercase", 0xffffffffu); uint64_t lower = hash(pixels);
    clear(pixels); x4_text_px(pixels, 0, 0, 32, 0, "LOWERCASE", 0xffffffffu); CHECK(hash(pixels) != lower);
    x4_text_px(pixels, -100, -100, 32, 0, spanish, 0xffffffffu);
    x4_text_px(pixels, INT_MAX, INT_MAX, INT_MAX, 1, spanish, 0xffffffffu);
    x4_text_px(pixels, INT_MIN, INT_MIN, INT_MIN, 0, spanish, 0xffffffffu);
    x4_text_px(NULL, 0, 0, 32, 0, spanish, 0xffffffffu);
    CHECK(allocation[0] == 0x49afee13u && allocation[WIDTH * HEIGHT + 1u] == 0x49afee13u);
    CHECK(x4_font_init("/missing/font/asset.x4font") == -1);
    CHECK(x4_font_is_fallback()); CHECK(x4_font_heap_bytes() == 0);
    CHECK(x4_font_init(argv[1]) == 0);
    /* A truncated/untrusted atlas must fail closed while preserving smooth
     * fallback text. No large allocation is needed to reject its header. */
    char invalid_path[1024];
    if (argc > 3) snprintf(invalid_path, sizeof(invalid_path), "%s", argv[3]);
    else snprintf(invalid_path, sizeof(invalid_path), "%s.invalid-test", argv[1]);
    FILE *bad = fopen(invalid_path, "wb"); CHECK(bad != NULL);
    CHECK(fwrite("X4FNT01", 1, 7, bad) == 7); CHECK(fclose(bad) == 0);
    CHECK(x4_font_init(invalid_path) == -1); CHECK(x4_font_is_fallback());
    CHECK(remove(invalid_path) == 0);
    CHECK(x4_font_init(argv[1]) == 0);
    /* Both a direct file and its containing directory are supported. */
    char directory[1024]; snprintf(directory, sizeof(directory), "%s", argv[1]);
    char *slash = strrchr(directory, '/');
    if (slash) { *slash = 0; CHECK(x4_font_init(directory) == 0); }
    if (argc > 2) {
        clear(pixels);
        x4_text_px(pixels, 80, 64, 56, 1, "XCloud4", 0xff71dc25u);
        x4_text_px(pixels, 80, 165, 46, 1, "Jugados recientemente", 0xffffffffu);
        x4_text_px(pixels, 80, 250, 32, 0, "Tu pr\xc3\xb3ximo juego: carreras, aventura y acci\xc3\xb3n", 0xffdfd6ceu);
        x4_text_px(pixels, 80, 320, 28, 0, "Espa\xc3\xb1ol: \xc3\xa1 \xc3\xa9 \xc3\xad \xc3\xb3 \xc3\xba \xc3\xb1 \xc3\xbc \xc2\xbf \xc2\xa1", 0xffffffffu);
        x4_text_ellipsis_px(pixels, 80, 390, 32, 1, "Una aventura extraordinariamente larga en M\xc3\xa9xico y m\xc3\xa1s all\xc3\xa1", 620, 0xffffffffu);
        x4_text_px(pixels, 80, 460, 26, 0, "Con acceso   Por confirmar   Todos", 0xff71dc25u);
        x4_text_px(pixels, 80, 530, 18, 0, "Regular / SemiBold: fuente suavizada desde el atlas real", 0xffdfd6ceu);
        ppm(argv[2], pixels);
    }
    x4_font_close(); CHECK(x4_font_heap_bytes() == 0);
    free(allocation); printf("font_test PASS: %u checks\n", checks); return 0;
}
