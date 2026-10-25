/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/canvas.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#include "../src/ui/screen.h"
#include "../src/input/touch_menu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_ppm(const char *path, const uint32_t *p)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n1920 1080\n255\n");
    for (size_t i = 0; i < 1920 * 1080; ++i) {
        uint8_t rgb[3] = {
            (uint8_t)(p[i] & 0xff),
            (uint8_t)((p[i] >> 8) & 0xff),
            (uint8_t)((p[i] >> 16) & 0xff)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out_dir = argc > 1 ? argv[1] : ".";
    const char *font_path = argc > 2 ? argv[2] : "assets/fonts/noto_sans.x4font";

    int font_rc = x4_font_init(font_path);
    assert(font_rc == 0);

    uint32_t *fb = calloc(1920 * 1080, sizeof(uint32_t));
    assert(fb);

    /* Background: simulate gameplay frame */
    for (int y = 0; y < 1080; ++y) {
        for (int x = 0; x < 1920; ++x) {
            uint32_t r = 20 + (x * 30 / 1920);
            uint32_t g = 25 + (y * 20 / 1080);
            uint32_t b = 35 + ((x + y) * 25 / 3000);
            fb[y * 1920 + x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    }

    /* 1. Spanish public menu */
    x4_lang_set(X4_LANG_ES, "es-ES", "ES");
    X4TouchMenu menu_es;
    x4_touch_menu_init(&menu_es, true);
    menu_es.is_open = true;
    menu_es.selected_index = 0;

    uint32_t *fb_es = malloc(1920 * 1080 * sizeof(uint32_t));
    memcpy(fb_es, fb, 1920 * 1080 * sizeof(uint32_t));
    x4_touch_menu_draw(&menu_es, 0, fb_es);

    char path_es[512];
    snprintf(path_es, sizeof(path_es), "%s/menu_juego_es.ppm", out_dir);
    write_ppm(path_es, fb_es);
    free(fb_es);

    /* 2. English public menu */
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    X4TouchMenu menu_en;
    x4_touch_menu_init(&menu_en, true);
    menu_en.is_open = true;
    menu_en.selected_index = 0;

    uint32_t *fb_en = malloc(1920 * 1080 * sizeof(uint32_t));
    memcpy(fb_en, fb, 1920 * 1080 * sizeof(uint32_t));
    x4_touch_menu_draw(&menu_en, 0, fb_en);

    char path_en[512];
    snprintf(path_en, sizeof(path_en), "%s/menu_juego_en.ppm", out_dir);
    write_ppm(path_en, fb_en);
    free(fb_en);

    /* 3. Spanish lab menu */
    x4_lang_set(X4_LANG_ES, "es-ES", "ES");
    X4TouchMenu menu_lab_es;
    x4_touch_menu_init(&menu_lab_es, false);
    menu_lab_es.is_open = true;
    menu_lab_es.selected_index = 0;

    uint32_t *fb_lab_es = malloc(1920 * 1080 * sizeof(uint32_t));
    memcpy(fb_lab_es, fb, 1920 * 1080 * sizeof(uint32_t));
    x4_touch_menu_draw(&menu_lab_es, 0, fb_lab_es);

    char path_lab_es[512];
    snprintf(path_lab_es, sizeof(path_lab_es), "%s/menu_juego_lab_es.ppm", out_dir);
    write_ppm(path_lab_es, fb_lab_es);
    free(fb_lab_es);

    /* 4. English lab menu */
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    X4TouchMenu menu_lab_en;
    x4_touch_menu_init(&menu_lab_en, false);
    menu_lab_en.is_open = true;
    menu_lab_en.selected_index = 0;

    uint32_t *fb_lab_en = malloc(1920 * 1080 * sizeof(uint32_t));
    memcpy(fb_lab_en, fb, 1920 * 1080 * sizeof(uint32_t));
    x4_touch_menu_draw(&menu_lab_en, 0, fb_lab_en);

    char path_lab_en[512];
    snprintf(path_lab_en, sizeof(path_lab_en), "%s/menu_juego_lab_en.ppm", out_dir);
    write_ppm(path_lab_en, fb_lab_en);
    free(fb_lab_en);

    /* 5. 6-second notice banner (Spanish) */
    x4_lang_set(X4_LANG_ES, "es-ES", "ES");
    X4TouchMenu menu_closed;
    x4_touch_menu_init(&menu_closed, true);
    X4LiveMediaSnapshot media = {0};
    X4SessionSnapshot sess = {0};
    uint32_t *fb_banner = malloc(1920 * 1080 * sizeof(uint32_t));
    memcpy(fb_banner, fb, 1920 * 1080 * sizeof(uint32_t));
    x4_live_overlay(&media, &sess, 0, &menu_closed, true, fb_banner);

    char path_banner[512];
    snprintf(path_banner, sizeof(path_banner), "%s/aviso_banner_6s.ppm", out_dir);
    write_ppm(path_banner, fb_banner);
    free(fb_banner);

    free(fb);
    x4_font_close();
    printf("PASS capture_menu_screens\n");
    return 0;
}
