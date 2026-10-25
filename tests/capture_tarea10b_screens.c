/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/canvas.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#include "../src/ui/screen.h"
#include "../src/auth/release_check.h"
#include "../src/auth/http_client.h"
#include "../src/auth/device_auth.h"
#include "../src/input/controller.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host stubs for http client in screen capture */
X4Http *x4_http_open(int *err) { if (err) *err = 0; return NULL; }
void x4_http_close(X4Http *h) { (void)h; }
int x4_http_releases_request(X4Http *h, const char *url, char *body, size_t cap,
    size_t *len, int *status, const _Atomic int *cancel)
{
    (void)h; (void)url; (void)body; (void)cap; (void)len; (void)status; (void)cancel;
    return -1;
}

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

    X4Controller ctrl = {0};
    ctrl.data.connected = 1;

    char path[512];

    /* 1. Main menu with update banner (ES & EN) */
    x4_release_check_set_mock(X4_UPDATE_AVAILABLE, "1.1.1-beta");

    /* Spanish Home */
    x4_lang_set(X4_LANG_ES, "es-ES", "ES");
    X4Screen screen_home = {.page = 0, .selected = 0};
    memset(fb, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_screen_draw(&screen_home, &ctrl, fb);
    snprintf(path, sizeof(path), "%s/menu_aviso_es.ppm", out_dir);
    write_ppm(path, fb);

    /* English Home */
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    memset(fb, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_screen_draw(&screen_home, &ctrl, fb);
    snprintf(path, sizeof(path), "%s/menu_aviso_en.ppm", out_dir);
    write_ppm(path, fb);

    /* 2. About page in each of the 4 states (ES & EN) */
    X4Screen screen_about = {.page = 2, .selected = 0};

    struct {
        enum X4UpdateStatus status;
        const char *version;
        const char *tag;
    } states[] = {
        { X4_UPDATE_CHECKING, NULL, "checking" },
        { X4_UPDATE_LATEST, NULL, "latest" },
        { X4_UPDATE_AVAILABLE, "1.1.1-beta", "available" },
        { X4_UPDATE_FAILED, NULL, "failed" }
    };

    for (int i = 0; i < 4; ++i) {
        x4_release_check_set_mock(states[i].status, states[i].version);

        /* Spanish */
        x4_lang_set(X4_LANG_ES, "es-ES", "ES");
        memset(fb, 0, 1920 * 1080 * sizeof(uint32_t));
        x4_screen_draw(&screen_about, &ctrl, fb);
        snprintf(path, sizeof(path), "%s/acerca_%s_es.ppm", out_dir, states[i].tag);
        write_ppm(path, fb);

        /* English */
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        memset(fb, 0, 1920 * 1080 * sizeof(uint32_t));
        x4_screen_draw(&screen_about, &ctrl, fb);
        snprintf(path, sizeof(path), "%s/acerca_%s_en.ppm", out_dir, states[i].tag);
        write_ppm(path, fb);
    }

    /* 3. Auth screen with QR (ES & EN) */
    X4AuthSnapshot auth_snap = {
        .state = X4_AUTH_WAITING,
        .seconds_left = 899,
        .user_code = "ABCD-1234",
        .verification_uri = "https://www.microsoft.com/link"
    };

    /* Spanish Auth */
    x4_lang_set(X4_LANG_ES, "es-ES", "ES");
    memset(fb, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_screen_draw(&screen_home, &ctrl, fb); /* background header */
    x4_auth_draw(&auth_snap, 0, 0, fb);
    snprintf(path, sizeof(path), "%s/auth_qr_es.ppm", out_dir);
    write_ppm(path, fb);

    /* English Auth */
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    memset(fb, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_screen_draw(&screen_home, &ctrl, fb); /* background header */
    x4_auth_draw(&auth_snap, 0, 0, fb);
    snprintf(path, sizeof(path), "%s/auth_qr_en.ppm", out_dir);
    write_ppm(path, fb);

    free(fb);
    x4_font_close();
    printf("PASS capture_tarea10b_screens\n");
    return 0;
}
