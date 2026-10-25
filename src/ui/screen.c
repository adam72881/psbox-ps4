/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include "canvas.h"
#include "font.h"
#include "lang.h"
#include "../auth/auth_profile.h"
#include "../core/build_identity.h"
#include "stream_video_status.h"
#include "../../third_party/qrcodegen/qrcodegen.h"
#include "../auth/release_check.h"
#include <stdio.h>
#include <string.h>

static bool auth_qr_code_valid(const char *code)
{
    if (!code || !code[0]) return false;
    for (const char *p = code; *p; ++p) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

static bool auth_qr_uri_valid(const char *uri)
{
    if (!uri) return false;
    static const char *const allowed[] = {
        "https://microsoft.com/devicelogin",
        "https://www.microsoft.com/link",
        "https://www.microsoft.com/devicelogin",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (!strcmp(uri, allowed[i])) return true;
    }
    return false;
}

bool x4_auth_qr_payload(const char *uri, const char *code, char *payload_out, size_t cap)
{
    if (!auth_qr_uri_valid(uri) || !auth_qr_code_valid(code)) return false;
    if (payload_out && cap) {
        snprintf(payload_out, cap, "%s?otc=%s", uri, code);
    }
    return true;
}
static const uint32_t BG = 0xff1b1510, PANEL = 0xff30261c;
static const uint32_t WHITE = 0xfff5f1eb, MUTED = 0xffb6a899, GREEN = 0xff72d86e;
void x4_screen_update(X4Screen *s, uint32_t pressed, uint32_t held)
{
#if !X4_PUBLIC
    if (pressed & ORBIS_PAD_BUTTON_OPTIONS) s->exit_requested = 1;
#endif
    /* Main waits for the session worker to close the remote session. */
    if (s->page == 6) return;
#if X4_PUBLIC
    (void)held;
    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
        s->page = s->page == 5 ? 4 : 0; return;
    }
    if (s->page == 0) {
        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_UP)) s->selected = (s->selected + 2) % 3;
        if (pressed & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_DOWN)) s->selected = (s->selected + 1) % 3;
        if (pressed & ORBIS_PAD_BUTTON_CROSS)
            s->page = s->selected == 0 ? 4 : (s->selected == 1 ? 2 : 7);
    }
#else
    if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) && (s->page != 1 || (held & ORBIS_PAD_BUTTON_L1))) {
        s->page = s->page == 5 ? 4 : 0; return;
    }
    if (s->page == 0) {
        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_UP)) s->selected = (s->selected + 3) % 4;
        if (pressed & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_DOWN)) s->selected = (s->selected + 1) % 4;
        if (pressed & ORBIS_PAD_BUTTON_CROSS) s->page = s->selected + 1;
    }
#endif
}
static void header(uint32_t *p, const X4Controller *c)
{
    x4_rect(p, 0, 0, X4_WIDTH, X4_HEIGHT, BG);
    x4_rect(p, 84, 88, 12, 78, GREEN);
    x4_text(p, 122, 92, 9, "XCloud4", WHITE);
    x4_text(p, 1420, 110, 3, X4_PRODUCT_VERSION, MUTED);
    x4_rect(p, 84, 205, 1752, 2, PANEL);
    x4_text(p, 84, 963, 3, c->data.connected ? x4_tr(X4_TR_DS4_CONNECTED) : x4_tr(X4_TR_DS4_CONNECT), c->data.connected ? GREEN : MUTED);
    if (c->error) {
        char error[50];
        snprintf(error, sizeof(error), x4_tr(X4_TR_CONTROLLER_ERR_FMT), (unsigned)c->error);
        x4_text(p, 1110, 963, 3, error, MUTED);
    }
}
static void home(uint32_t *p, const X4Screen *s)
{
#if X4_PUBLIC
    x4_text(p, 84, 267, 5, x4_tr(X4_TR_HOME_TITLE), WHITE);
    x4_text(p, 84, 332, 3, x4_tr(X4_TR_HOME_SUBTITLE_PUBLIC), MUTED);
    char update_ver[32] = {0};
    if (x4_release_check_status(update_ver, sizeof(update_ver)) == X4_UPDATE_AVAILABLE && update_ver[0]) {
        char banner[128];
        snprintf(banner, sizeof(banner), x4_tr(X4_TR_HOME_UPDATE_AVAILABLE_FMT), update_ver);
        x4_text(p, 84, 388, 3, banner, GREEN);
    }
    const char *titles[] = {
        x4_tr(X4_TR_CARD_ACCOUNT_TITLE),
        x4_tr(X4_TR_CARD_ABOUT_TITLE),
        x4_tr(X4_TR_CARD_SUPPORT_TITLE)
    };
    const char *descriptions[] = {
        x4_tr(X4_TR_CARD_ACCOUNT_DESC),
        x4_tr(X4_TR_CARD_ABOUT_DESC),
        x4_tr(X4_TR_CARD_SUPPORT_DESC)
    };
    for (int i = 0; i < 3; ++i) {
        int x = 84 + i * 604;
        x4_rect(p, x, 441, 544, 326, PANEL);
        x4_rect(p, x, 441, 544, 6, s->selected == i ? GREEN : PANEL);
        x4_text(p, x + 28, 493, 4, titles[i], WHITE);
        x4_text(p, x + 28, 590, 3, descriptions[i], MUTED);
        x4_text(p, x + 28, 699, 3, s->selected == i ? x4_tr(X4_TR_X_OPEN) : "", GREEN);
    }
    x4_text(p, 84, 833, 3, x4_tr(X4_TR_HOME_HINT), MUTED);
    x4_text(p, 84, 894, 3, x4_tr(X4_TR_HOME_FOOTER_PUBLIC), WHITE);
#else
    x4_text(p, 84, 267, 5, x4_tr(X4_TR_HOME_TITLE), WHITE);
    x4_text(p, 84, 332, 3, x4_tr(X4_TR_HOME_SUBTITLE_LAB), MUTED);
    char update_ver[32] = {0};
    if (x4_release_check_status(update_ver, sizeof(update_ver)) == X4_UPDATE_AVAILABLE && update_ver[0]) {
        char banner[128];
        snprintf(banner, sizeof(banner), x4_tr(X4_TR_HOME_UPDATE_AVAILABLE_FMT), update_ver);
        x4_text(p, 84, 388, 3, banner, GREEN);
    }
    const char *titles[] = {"CONTROL", "PROYECTO", "IMAGEN Y\nSONIDO", x4_tr(X4_TR_CARD_ACCOUNT_TITLE)};
    const char *descriptions[] = {"BOTONES, PALANCAS\nY GATILLOS.", "ESTADO Y SIGUIENTES\nETAPAS DEL PROYECTO.", "UN VIDEO LOCAL\nY TONOS EN ESTEREO.", x4_tr(X4_TR_CARD_ACCOUNT_DESC)};
    for (int i = 0; i < 4; ++i) {
        int x = 84 + i * 446;
        x4_rect(p, x, 441, 414, 326, PANEL);
        x4_rect(p, x, 441, 414, 6, s->selected == i ? GREEN : PANEL);
        x4_text(p, x + 28, 493, 4, titles[i], WHITE);
        x4_text(p, x + 28, 590, 3, descriptions[i], MUTED);
        x4_text(p, x + 28, 699, 3, s->selected == i ? x4_tr(X4_TR_X_OPEN) : "", GREEN);
    }
    x4_text(p, 84, 833, 3, x4_tr(X4_TR_HOME_HINT), MUTED);
    x4_text(p, 84, 894, 3, x4_tr(X4_TR_HOME_FOOTER_LAB), WHITE);
#endif
}
#if !X4_PUBLIC
static void button(uint32_t *p, int x, int y, const char *label, uint32_t mask, uint32_t buttons)
{
    int active = (buttons & mask) != 0;
    x4_rect(p, x, y, 230, 70, active ? GREEN : PANEL);
    x4_text(p, x + 18, y + 23, 3, label, active ? BG : WHITE);
}
static void stick_panel(uint32_t *p, int x, int y, const char *label, stick value)
{
    x4_text(p, x, y, 3, label, WHITE);
    x4_rect(p, x, y + 42, 230, 150, PANEL);
    x4_rect(p, x + 114, y + 50, 2, 134, MUTED);
    x4_rect(p, x + 8, y + 115, 214, 2, MUTED);
    x4_rect(p, x + 8 + (int)value.x * 204 / 255, y + 50 + (int)value.y * 124 / 255, 10, 10, GREEN);
    char text[32];
    snprintf(text, sizeof(text), "X %03u  Y %03u", (unsigned)value.x, (unsigned)value.y);
    x4_text(p, x, y + 215, 3, text, MUTED);
}
static void controller_page(uint32_t *p, const X4Controller *c)
{
    x4_text(p, 84, 267, 5, "TU CONTROL", WHITE);
    x4_text(p, 84, 332, 3, "PULSA UN BOTON O MUEVE LAS PALANCAS.", MUTED);
    const char *labels[] = {"ARRIBA", "DERECHA", "ABAJO", "IZQUIERDA", "TRIANGULO", "CIRCULO", "X", "CUADRADO", "L1", "R1", "L3", "R3", "L2", "R2", "TOUCHPAD"};
    const uint32_t masks[] = {ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_RIGHT, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_LEFT,
        ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1, ORBIS_PAD_BUTTON_L3, ORBIS_PAD_BUTTON_R3,
        ORBIS_PAD_BUTTON_L2, ORBIS_PAD_BUTTON_R2, ORBIS_PAD_BUTTON_TOUCH_PAD};
    for (int i = 0; i < 15; ++i)
        button(p, 84 + (i % 4) * 250, 410 + (i / 4) * 91, labels[i], masks[i], c->data.buttons);
    stick_panel(p, 1160, 410, "IZQUIERDA", c->data.leftStick);
    stick_panel(p, 1500, 410, "DERECHA", c->data.rightStick);
    char triggers[48];
    snprintf(triggers, sizeof(triggers), "L2 %03u     R2 %03u", (unsigned)c->data.analogButtons.l2, (unsigned)c->data.analogButtons.r2);
    x4_text(p, 1160, 713, 3, triggers, WHITE);
    x4_rect(p, 1160, 758, 255, 12, PANEL);
    x4_rect(p, 1160, 758, c->data.analogButtons.l2, 12, GREEN);
    x4_rect(p, 1500, 758, 255, 12, PANEL);
    x4_rect(p, 1500, 758, c->data.analogButtons.r2, 12, GREEN);
#if X4_PUBLIC
    x4_text(p, 84, 894, 3, "L1 + CIRCULO  VOLVER", WHITE);
#else
    x4_text(p, 84, 894, 3, "L1 + CIRCULO  VOLVER     OPTIONS  SALIR", WHITE);
#endif
}
#endif
static void draw_update_status(uint32_t *p, int x, int y)
{
    char ver[32] = {0};
    enum X4UpdateStatus st = x4_release_check_status(ver, sizeof(ver));
    char line[128];
    uint32_t color = MUTED;
    switch (st) {
    case X4_UPDATE_CHECKING:
        snprintf(line, sizeof(line), "%s", x4_tr(X4_TR_UPDATE_CHECKING));
        color = MUTED;
        break;
    case X4_UPDATE_LATEST:
        snprintf(line, sizeof(line), "%s", x4_tr(X4_TR_UPDATE_LATEST));
        color = WHITE;
        break;
    case X4_UPDATE_AVAILABLE:
        snprintf(line, sizeof(line), x4_tr(X4_TR_UPDATE_AVAILABLE_FMT), ver);
        color = GREEN;
        break;
    case X4_UPDATE_FAILED:
    default:
        snprintf(line, sizeof(line), "%s", x4_tr(X4_TR_UPDATE_FAILED));
        color = MUTED;
        break;
    }
    x4_text(p, x, y, 2, line, color);
}

static void project_page(uint32_t *p)
{
#if X4_PUBLIC
    x4_text(p, 84, 245, 5, x4_tr(X4_TR_ABOUT_TITLE), WHITE);
    x4_rect(p, 84, 316, 1752, 540, PANEL);
    x4_text(p, 126, 344, 3, x4_tr(X4_TR_ABOUT_LINE1), GREEN);
    x4_text(p, 126, 408, 2, x4_tr(X4_TR_ABOUT_LINE2), WHITE);
    x4_text(p, 126, 464, 2, x4_tr(X4_TR_ABOUT_LINE3), MUTED);
    x4_text(p, 126, 520, 2, x4_tr(X4_TR_ABOUT_LINE4), WHITE);
    x4_text(p, 126, 576, 2, x4_tr(X4_TR_ABOUT_LINE5), GREEN);
    x4_text(p, 126, 632, 2, x4_tr(X4_TR_ABOUT_LINE6), MUTED);
    x4_text(p, 126, 704, 2, x4_tr(X4_TR_ABOUT_LINE7), WHITE);
    x4_text(p, 126, 760, 2, x4_tr(X4_TR_ABOUT_LINE8), MUTED);
    draw_update_status(p, 126, 812);
    x4_text(p, 84, 894, 3, x4_tr(X4_TR_CIRCLE_BACK), WHITE);
#else
    x4_text(p, 84, 267, 5, "UN CLIENTE NATIVO PARA PS4", WHITE);
    x4_text(p, 84, 356, 3, "OBJETIVO: JUGAR DESDE LA CONSOLA, DIRECTO A XBOX CLOUD GAMING.", MUTED);
    x4_rect(p, 84, 445, 1752, 356, PANEL);
    x4_text(p, 126, 490, 4, "AHORA", GREEN);
    x4_text(p, 126, 550, 3, "CONTROL, IMAGEN Y SONIDO. CUENTA MICROSOFT Y CATALOGO.", WHITE);
    x4_text(p, 126, 625, 4, "DESPUES", WHITE);
    x4_text(p, 126, 685, 3, "CONEXION A UN JUEGO. IMAGEN, SONIDO Y CONTROL EN LA NUBE.", MUTED);
    draw_update_status(p, 126, 748);
    x4_text(p, 84, 894, 3, "CIRCULO  VOLVER     OPTIONS  SALIR", WHITE);
#endif
}
#if X4_PUBLIC
static void text_wrapped(uint32_t *p, int x, int y, int scale, size_t wrap_col, const char *text, uint32_t color)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", text);
    size_t len = strlen(buf);
    if (len > wrap_col) {
        size_t split = wrap_col;
        while (split > 0 && buf[split] != ' ') --split;
        if (split > 0) buf[split] = '\n';
    }
    x4_text(p, x, y, scale, buf, color);
}
static void support_page(uint32_t *p)
{
    static uint8_t qr_cdata[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t qr_temp[qrcodegen_BUFFER_LEN_MAX];
    static int qr_ready = 0;
    static int qr_valid = 0;
    if (!qr_ready) {
        qr_valid = qrcodegen_encodeText("https://ko-fi.com/asapedgr",
            qr_temp, qr_cdata, qrcodegen_Ecc_MEDIUM,
            qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
            qrcodegen_Mask_AUTO, false) ? 1 : 0;
        qr_ready = 1;
    }
    x4_text(p, 84, 245, 5, x4_tr(X4_TR_SUPPORT_TITLE), WHITE);
    x4_rect(p, 84, 316, 1752, 540, PANEL);
    text_wrapped(p, 126, 356, 2, 62, x4_tr(X4_TR_SUPPORT_LINE1), WHITE);
    text_wrapped(p, 126, 460, 2, 62, x4_tr(X4_TR_SUPPORT_LINE2), WHITE);
    x4_text(p, 126, 572, 2, x4_tr(X4_TR_SUPPORT_LINE3), MUTED);
    x4_text(p, 126, 648, 2, x4_tr(X4_TR_SUPPORT_LINE4), GREEN);
    x4_text(p, 126, 736, 3, x4_tr(X4_TR_SUPPORT_LINE5), WHITE);
    if (qr_valid) {
        int qr_size = qrcodegen_getSize(qr_cdata);
        int margin = 4;
        int total_modules = qr_size + margin * 2;
        int mod_px = 462 / total_modules;
        int box_px = total_modules * mod_px;
        int qr_x = 84 + 1752 - 40 - box_px;
        int qr_y = 316 + (540 - box_px) / 2;
        x4_rect(p, qr_x, qr_y, box_px, box_px, 0xffffffffu);
        for (int my = 0; my < qr_size; ++my) {
            for (int mx = 0; mx < qr_size; ++mx) {
                if (qrcodegen_getModule(qr_cdata, mx, my)) {
                    x4_rect(p, qr_x + (margin + mx) * mod_px,
                        qr_y + (margin + my) * mod_px, mod_px, mod_px, 0xff000000u);
                }
            }
        }
    }
    x4_text(p, 84, 894, 3, x4_tr(X4_TR_CIRCLE_BACK), WHITE);
}
#endif
void x4_screen_draw(const X4Screen *s, const X4Controller *c, uint32_t *p)
{
    header(p, c);
#if X4_PUBLIC
    if (s->page == 2) project_page(p);
    else if (s->page == 3 || s->page == 7) support_page(p);
    else if (s->page == 0) home(p, s);
#else
    if (s->page == 1) controller_page(p, c);
    else if (s->page == 2) project_page(p);
    else if (s->page == 0) home(p, s);
#endif
}

void x4_media_draw(const X4DemoVideo *v, const X4DemoAudio *a, uint32_t *p)
{
    x4_text(p, 84, 260, 5, "IMAGEN Y SONIDO", WHITE);
    x4_rect(p, 82, 328, 964, 556, PANEL);
    if (v->pixels && v->frames) {
        for (unsigned y = 0; y < 552; ++y)
            for (unsigned x = 0; x < 960; ++x)
                p[(330 + y) * X4_WIDTH + 84 + x] = v->pixels[(y * 2 / 3) * X4_SAMPLE_WIDTH + x * 2 / 3];
    } else {
        x4_text(p, 126, 529, 3, v->error ? "VIDEO NO DISPONIBLE" : "INICIANDO VIDEO LOCAL...", MUTED);
    }
    x4_text(p, 1110, 355, 4, "VIDEO H264", v->error ? MUTED : GREEN);
    x4_text(p, 1110, 417, 2, v->stage[0] ? v->stage : "INICIANDO...", WHITE);
    char line[64];
    if (v->error) snprintf(line, sizeof(line), "ERROR 0X%08X", (unsigned)v->error);
    else snprintf(line, sizeof(line), "IMAGENES %u / %u", v->frames, v->total);
    x4_text(p, 1110, 460, 3, line, MUTED);
    x4_text(p, 1110, 536, 4, "SONIDO ESTEREO", atomic_load(&a->error) ? MUTED : GREEN);
    int error = atomic_load(&a->error);
    if (error) snprintf(line, sizeof(line), "ERROR 0X%08X", (unsigned)error);
    else snprintf(line, sizeof(line), "%s", atomic_load(&a->muted) ? "SILENCIADO" : atomic_load(&a->finished) ? "TONOS TERMINADOS" : "TONOS IZQUIERDA / DERECHA");
    x4_text(p, 1110, 598, 2, line, WHITE);
    x4_text(p, 1110, 710, 2, "MUESTRA LOCAL DE 8 SEGUNDOS.\n640 X 368. 48 KHZ.\nXBOX AUN NO ESTA CONECTADO.", MUTED);
#if X4_PUBLIC
    x4_text(p, 84, 915, 3, "X  REPETIR     CUADRADO  SONIDO     CIRCULO  VOLVER", WHITE);
#else
    x4_text(p, 84, 915, 3, "X  REPETIR     CUADRADO  SONIDO     CIRCULO  VOLVER     OPTIONS  SALIR", WHITE);
#endif
}

void x4_exit_error_draw(int error, uint32_t *p)
{
    if (!error) return;
    char message[96];
#if X4_PUBLIC
    snprintf(message, sizeof(message), x4_tr(X4_TR_EXIT_ERR_PUBLIC_FMT), (unsigned)error);
#else
    snprintf(message, sizeof(message), x4_tr(X4_TR_EXIT_ERR_LAB_FMT), (unsigned)error);
#endif
    x4_rect(p, 84, 999, 1752, 54, PANEL);
    x4_text(p, 100, 1015, 3, message, WHITE);
}

void x4_auth_draw(const X4AuthSnapshot *a, int busy, int closing, uint32_t *p)
{
    x4_text(p, 84, 267, 5, x4_tr(X4_TR_AUTH_TITLE), WHITE);
#if X4_PUBLIC
    x4_text(p, 84, 332, 3, x4_tr(X4_TR_AUTH_PROMPT_PUBLIC), MUTED);
#else
    x4_text(p, 84, 332, 3, X4_AUTH_PROFILE_PROMPT, MUTED);
#endif
    x4_rect(p, 84, 400, 1752, 424, PANEL);
    if (closing) {
        x4_text(p, 126, 452, 4, x4_tr(X4_TR_AUTH_CLOSING_TITLE), WHITE);
        x4_text(p, 126, 530, 3, x4_tr(X4_TR_AUTH_CLOSING_SUB), MUTED);
    } else if (a->state == X4_AUTH_WAITING) {
        x4_text(p, 126, 443, 3, x4_tr(X4_TR_AUTH_CODE_PROMPT), WHITE);
        x4_text(p, 126, 493, strlen(a->user_code) <= 16 ? 8 : 4, a->user_code, GREEN);
        x4_text_literal(p, 126, 593, 3, a->verification_uri, WHITE);
        char left[64];
        snprintf(left, sizeof(left), x4_tr(X4_TR_AUTH_EXPIRES_FMT), a->seconds_left / 60, a->seconds_left % 60);
        x4_text(p, 126, 661, 3, left, MUTED);
        x4_text(p, 126, 734, 3, x4_tr(X4_TR_AUTH_CANCEL_HINT), MUTED);

        static uint8_t auth_qr_cdata[qrcodegen_BUFFER_LEN_MAX];
        static uint8_t auth_qr_temp[qrcodegen_BUFFER_LEN_MAX];
        static char cached_code[32] = "";
        static char cached_uri[256] = "";
        static int auth_qr_valid = 0;

        if (strcmp(cached_code, a->user_code) != 0 || strcmp(cached_uri, a->verification_uri) != 0) {
            snprintf(cached_code, sizeof(cached_code), "%s", a->user_code);
            snprintf(cached_uri, sizeof(cached_uri), "%s", a->verification_uri);
            auth_qr_valid = 0;
            char qr_payload[512];
            if (x4_auth_qr_payload(a->verification_uri, a->user_code, qr_payload, sizeof(qr_payload))) {
                auth_qr_valid = qrcodegen_encodeText(qr_payload, auth_qr_temp, auth_qr_cdata,
                    qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                    qrcodegen_Mask_AUTO, false) ? 1 : 0;
            }
        }

        if (auth_qr_valid) {
            int qr_size = qrcodegen_getSize(auth_qr_cdata);
            int margin = 4;
            int total_modules = qr_size + margin * 2;
            int target_px = 320;
            int mod_px = target_px / total_modules;
            if (mod_px < 1) mod_px = 1;
            int box_px = total_modules * mod_px;
            int qr_x = 84 + 1752 - 80 - box_px;
            int qr_y = 400 + (424 - box_px - 36) / 2;
            x4_rect(p, qr_x, qr_y, box_px, box_px, 0xffffffffu);
            for (int my = 0; my < qr_size; ++my) {
                for (int mx = 0; mx < qr_size; ++mx) {
                    if (qrcodegen_getModule(auth_qr_cdata, mx, my)) {
                        x4_rect(p, qr_x + (margin + mx) * mod_px,
                            qr_y + (margin + my) * mod_px, mod_px, mod_px, 0xff000000u);
                    }
                }
            }
            const char *scan_label = x4_tr(X4_TR_AUTH_SCAN_PHONE);
            int text_w = x4_text_width_px(26, 0, scan_label);
            int text_x = qr_x + (box_px - text_w) / 2;
            x4_text(p, text_x, qr_y + box_px + 12, 2, scan_label, WHITE);
        }
    } else {
        const char *title = x4_tr(X4_TR_AUTH_READY);
        switch (a->state) {
        case X4_AUTH_CONNECTING: title = x4_tr(X4_TR_AUTH_CONNECTING); break;
        case X4_AUTH_CONNECTED: title = x4_tr(X4_TR_AUTH_CONNECTED); break;
        case X4_AUTH_AUTHORIZED: title = x4_tr(X4_TR_AUTH_AUTHORIZED); break;
        case X4_AUTH_CANCELLED: title = x4_tr(X4_TR_AUTH_CANCELLED); break;
        case X4_AUTH_EXPIRED: title = x4_tr(X4_TR_AUTH_EXPIRED); break;
        case X4_AUTH_DENIED: title = x4_tr(X4_TR_AUTH_DENIED); break;
        case X4_AUTH_ERROR: title = x4_tr(X4_TR_AUTH_ERROR); break;
        default: break;
        }
        x4_text(p, 126, 452, 4, title, a->state == X4_AUTH_ERROR ? WHITE : GREEN);
        x4_text(p, 126, 531, 3, x4_tr_stage(a->stage), MUTED);
        if (a->state == X4_AUTH_AUTHORIZED)
            x4_text(p, 126, 601, 3, x4_tr(X4_TR_AUTH_AUTHORIZED_HINT), WHITE);
        else if (!busy)
#if X4_PUBLIC
            x4_text(p, 126, 601, 3, x4_tr(X4_TR_AUTH_IDLE_HINT_PUBLIC), WHITE);
#else
            x4_text(p, 126, 601, 3, x4_tr(X4_TR_AUTH_IDLE_HINT_LAB), WHITE);
#endif
        if (a->error || a->http_status >= 400) {
            char error[72];
            snprintf(error, sizeof(error), x4_tr(X4_TR_DETAIL_HTTP_FMT), (unsigned)a->error, a->http_status);
            x4_text(p, 126, 742, 3, error, MUTED);
        }
    }
#if X4_PUBLIC
    x4_text(p, 84, 853, 2, x4_tr(X4_TR_AUTH_FOOTER_PUBLIC), WHITE);
#else
    x4_text(p, 84, 853, 2, x4_tr(X4_TR_AUTH_FOOTER_LAB), WHITE);
#endif
    x4_text(p, 84, 899, 2, x4_tr(X4_TR_AUTH_R1_HINT), MUTED);
#if X4_PUBLIC
    x4_text(p, 84, 1004, 2, x4_tr(X4_TR_AUTH_NOTE_PUBLIC), MUTED);
#else
    x4_text(p, 84, 1004, 2, X4_AUTH_PROFILE_NOTE, MUTED);
#endif
}

void x4_session_draw(const X4SessionSnapshot *s, int busy, int closing, uint32_t *p)
{
    x4_text(p, 84, 260, 5, x4_tr(X4_TR_SESS_TITLE), WHITE);
    char line[160];
    snprintf(line, sizeof(line), "%.85s", s->title_name[0] ? s->title_name : x4_tr(X4_TR_SESS_SELECTED_TITLE));
    x4_text(p, 84, 328, 3, line, MUTED);
    x4_rect(p, 84, 400, 1752, 424, PANEL);
    const char *title = x4_tr(X4_TR_SESS_PREPARING);
    switch (s->state) {
    case X4_SESSION_WAITING: title = x4_tr(X4_TR_SESS_WAITING); break;
    case X4_SESSION_READY: title = x4_tr(X4_TR_SESS_READY); break;
    case X4_SESSION_AUTHORIZING: title = x4_tr(X4_TR_SESS_AUTHORIZING); break;
    case X4_SESSION_AUTHORIZED: title = x4_tr(X4_TR_SESS_AUTHORIZED); break;
    case X4_SESSION_NEGOTIATING: title = x4_tr(X4_TR_SESS_NEGOTIATING); break;
    case X4_SESSION_CONNECTING: title = x4_tr(X4_TR_SESS_CONNECTING); break;
    case X4_SESSION_STREAMING: title = x4_tr(X4_TR_SESS_STREAMING); break;
    case X4_SESSION_STOPPING: title = x4_tr(X4_TR_SESS_STOPPING); break;
    case X4_SESSION_CLOSED: title = x4_tr(X4_TR_SESS_CLOSED); break;
    case X4_SESSION_CANCELLED: title = x4_tr(X4_TR_SESS_CANCELLED); break;
    case X4_SESSION_ERROR: title = x4_tr(X4_TR_SESS_ERROR); break;
    default: break;
    }
    if (closing) title = x4_tr(X4_TR_SESS_CLOSING_APP);
    x4_text(p, 126, 448, 4, title, s->ready_seen && !s->cleanup_failed && s->state != X4_SESSION_ERROR ? GREEN : WHITE);
    x4_text(p, 126, 523, 3, x4_tr_stage(s->stage), MUTED);
    if (s->state == X4_SESSION_AUTHORIZED) {
        snprintf(line, sizeof(line), "%s", x4_tr(X4_TR_SESS_PREP_AV));
        x4_text(p, 126, 587, 3, line, WHITE);
    } else if (s->state == X4_SESSION_READY) {
        snprintf(line, sizeof(line), "%s", x4_tr(X4_TR_SESS_PREP_AUTH));
        x4_text(p, 126, 587, 3, line, WHITE);
    } else if (s->connection_authorized && !busy)
        x4_text(p, 126, 587, 3, x4_tr(X4_TR_SESS_PERM_ACCEPTED), GREEN);
    else if (s->ready_seen && !busy)
        x4_text(p, 126, 587, 3, x4_tr(X4_TR_SESS_CONFIRMED_READY), GREEN);
    else if (busy) {
        snprintf(line, sizeof(line), x4_tr(X4_TR_SESS_ELAPSED_FMT), s->elapsed_seconds);
        x4_text(p, 126, 587, 3, line, MUTED);
    }
    snprintf(line, sizeof(line), x4_tr(X4_TR_SESS_WEBRTC_FMT), s->sdp_http_status,
        s->ice_http_status, s->rtc_connected ? x4_tr(X4_TR_SESS_RTC_CONNECTED) : x4_tr(X4_TR_SESS_RTC_WAITING));
    x4_text(p, 126, 650, 3, line, MUTED);
    if (s->cleanup_failed) {
        snprintf(line, sizeof(line), x4_tr(X4_TR_SESS_CLEANUP_FAIL_FMT),
            (unsigned)s->cleanup_error, s->cleanup_http_status);
        x4_text(p, 126, 722, 3, line, WHITE);
    } else if (s->error || s->http_status >= 400) {
        snprintf(line, sizeof(line), x4_tr(X4_TR_SESS_DETAIL_FMT), (unsigned)s->error, s->http_status);
        x4_text(p, 126, 722, 3, line, WHITE);
    }
#if X4_PUBLIC
    x4_text(p, 84, 867, 3, x4_tr(X4_TR_SESS_FOOTER_PUBLIC), WHITE);
    x4_text(p, 84, 940, 2, x4_tr(X4_TR_GAME_TOUCH_HINT), MUTED);
#else
    x4_text(p, 84, 867, 3, x4_tr(X4_TR_SESS_FOOTER_LAB), WHITE);
    x4_text(p, 84, 940, 2, x4_tr(X4_TR_GAME_TOUCH_HINT), MUTED);
#endif
}

void x4_live_status_draw(const X4LiveMediaSnapshot *m, int error, uint32_t *p)
{
    char line[160];
    snprintf(line, sizeof(line), x4_tr(X4_TR_LIVE_PACKETS_FMT),
        (unsigned long long)m->video_packets, (unsigned long long)m->video_frames,
        (unsigned long long)m->audio_packets, (unsigned long long)m->audio_frames);
    x4_text(p, 126, 790, 2, line, m->video_ready ? GREEN : MUTED);
    if (error || m->video_error || m->audio_error) {
#if X4_STREAM_PROFILE
        if(m->video_error || error) {
            int video_error = m->video_error ? m->video_error : error;
            (void)x4_stream_video_error_text(line,sizeof(line),
                m->video_rejected_width,m->video_rejected_height,video_error);
            /* Replace the normal shortcut footer with the complete status;
             * its 26 px line stays inside TV-safe horizontal margins. */
            x4_rect(p,80,842,1760,112,BG);
            x4_text(p,84,850,2,line,WHITE);
#if X4_PUBLIC
            snprintf(line,sizeof(line),x4_tr(X4_TR_LIVE_VIDEO_ERR_PUBLIC_FMT),video_error,
                m->audio_error ? x4_tr(X4_TR_LIVE_AUDIO_UNAVAIL) : m->audio_playing ? x4_tr(X4_TR_LIVE_AUDIO_ACTIVE) : x4_tr(X4_TR_LIVE_AUDIO_WAITING));
#else
            snprintf(line,sizeof(line),x4_tr(X4_TR_LIVE_VIDEO_ERR_LAB_FMT),video_error,
                m->audio_error ? x4_tr(X4_TR_LIVE_AUDIO_UNAVAIL) : m->audio_playing ? x4_tr(X4_TR_LIVE_AUDIO_ACTIVE) : x4_tr(X4_TR_LIVE_AUDIO_WAITING));
#endif
            x4_text(p,84,891,2,line,WHITE);
            return;
        }
#endif
        snprintf(line, sizeof(line), x4_tr(X4_TR_LIVE_MEDIA_ERR_FMT), (unsigned)error,
            (unsigned)m->video_error, (unsigned)m->audio_error);
        x4_text(p, 84, 922, 2, line, WHITE);
    }
}
void x4_touch_menu_draw(const X4TouchMenu *menu, int muted, uint32_t *p)
{
    if (!menu || !menu->is_open) return;
    int mw = 760;
    int items = menu->item_count;
    int mh = 66 + (items * 54) + 60;
    int mx = (X4_WIDTH - mw) / 2;
    int my = (X4_HEIGHT - mh) / 2;

    /* Outer border and main modal card */
    x4_rect(p, mx - 3, my - 3, mw + 6, mh + 6, 0xff150f0bu);
    x4_rect(p, mx, my, mw, mh, 0xff281d16u);

    /* Header */
    x4_rect(p, mx, my, mw, 54, 0xff37291fu);
    x4_text(p, mx + 32, my + 6, 3, x4_tr(X4_TR_TOUCH_MENU_TITLE), WHITE);

    /* Options */
    for (int i = 0; i < items; ++i) {
        const char *text = "";
        if (i == 0) text = x4_tr(X4_TR_TOUCH_MENU_BACK);
        else if (i == 1) text = x4_tr(muted ? X4_TR_TOUCH_MENU_UNMUTE : X4_TR_TOUCH_MENU_MUTE);
        else if (i == 2) text = x4_tr(X4_TR_TOUCH_MENU_XBOX);
        else if (menu->is_public) {
            if (i == 3) text = x4_tr(X4_TR_TOUCH_MENU_CANCEL);
        } else {
            if (i == 3) text = x4_tr(X4_TR_TOUCH_MENU_MARK_TEST);
            else if (i == 4) text = x4_tr(X4_TR_TOUCH_MENU_EXIT_APP);
            else if (i == 5) text = x4_tr(X4_TR_TOUCH_MENU_CANCEL);
        }

        int iy = my + 66 + i * 54;
        if (i == menu->selected_index) {
            x4_rect(p, mx + 16, iy, mw - 32, 48, 0xff107c10u);
            x4_text(p, mx + 36, iy + 2, 3, text, WHITE);
        } else {
            x4_rect(p, mx + 16, iy, mw - 32, 48, 0xff30241cu);
            x4_text(p, mx + 36, iy + 2, 3, text, MUTED);
        }
    }

    /* Footer */
    x4_rect(p, mx + 16, my + mh - 48, mw - 32, 1, 0xff48372du);
    x4_text(p, mx + 32, my + mh - 36, 2, x4_tr(X4_TR_TOUCH_MENU_FOOTER), MUTED);
}

void x4_live_overlay(const X4LiveMediaSnapshot *m, const X4SessionSnapshot *s, int muted,
                     const X4TouchMenu *touch_menu, bool show_banner, uint32_t *p)
{
    /* 6-second banner when video playback starts */
    if (show_banner && (!touch_menu || !touch_menu->is_open)) {
        x4_rect(p, 0, X4_HEIGHT - 44, X4_WIDTH, 44, BG);
        x4_text(p, 48, X4_HEIGHT - 34, 2, x4_tr(X4_TR_GAME_TOUCH_HINT), WHITE);
    }
    if (m && s && (m->audio_error || s->input_error || muted)) {
        char line[96];
        const char *audio = m->audio_error ? x4_tr(X4_TR_OVERLAY_AUDIO_UNAVAIL) : muted ? x4_tr(X4_TR_OVERLAY_AUDIO_MUTED) : "";
        snprintf(line, sizeof(line), "%s%s%s", audio,
            audio[0] && s->input_error ? " / " : "",
            s->input_error ? x4_tr(X4_TR_OVERLAY_INPUT_UNAVAIL) : "");
        x4_rect(p, 20, 20, 620, 36, BG);
        x4_text(p, 32, 29, 2, line, WHITE);
    }
    if (touch_menu && touch_menu->is_open) {
        x4_touch_menu_draw(touch_menu, muted, p);
    }
}
