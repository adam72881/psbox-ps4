/* SPDX-License-Identifier: GPL-3.0-or-later
 * Decisione di installazione BGFT, condivisa dal payload e dal test host.
 * Il processo è GoldHEN, non l'app: user id 0 o -1 fa rifiutare il task.
 * Le stringhe sono puntatori vuoti, mai NULL (Itemzflow lascia alcuni NULL
 * solo perché gira come app dell'utente). Mai sceAppInstUtilAppInstallPkg:
 * sulla console torna 0x80020012 e il sistema dice che non è supportato.
 */
#ifndef PSBOX_INSTALL_PLAN_H
#define PSBOX_INSTALL_PLAN_H

#define XC_USER_ALREADY_INIT 0x80960003u
#define XC_BGFT_INVALID 0x80990004u
#define XC_BGFT_ALREADY 0x80990088u
#define XC_BGFT_EXISTS 0x80990015u
#define XC_BGFT_BUSY 0x80990086u
#define XC_APPINST_UNSUPPORTED 0x80020012u
#define XC_OPT_DISABLE_CDN 0x10000
#define XC_OPT_FORCE_UPDATE 0x8

#define XC_DRIVE_OK 0
#define XC_DRIVE_NO_USER (-2)
#define XC_DRIVE_BAD_ARG (-3)
#define XC_DRIVE_TIMEOUT (-4)
#define XC_DRIVE_STARTED_FAIL (-5)

enum {
    XC_STEP_REGISTER = 1,
    XC_STEP_PREPARE = 2,
    XC_STEP_START = 3,
    XC_STEP_POLL = 4,
    XC_STEP_DONE = 5,
    XC_STEP_FAIL = 6,
    XC_STEP_NO_USER = 7
};

typedef struct XcBgftParam {
    int user_id;
    int entitlement_type;
    const char* id;
    const char* content_url;
    const char* content_ex_url;
    const char* content_name;
    const char* icon_path;
    const char* sku_id;
    int option;
    const char* playgo_scenario_id;
    const char* release_date;
    const char* package_type;
    const char* package_sub_type;
    unsigned long package_size;
} XcBgftParam;

typedef struct XcBgftParamEx {
    XcBgftParam param;
    unsigned int slot;
} XcBgftParamEx;

typedef struct XcBgftProgPrefix {
    unsigned int bits;
    int error_result;
    unsigned long length;
    unsigned long transferred;
} XcBgftProgPrefix;

typedef char xc_bgft_size_check[(__builtin_offsetof(XcBgftParam, package_size) == 96) ? 1 : -1];
typedef char xc_bgft_slot_check[(__builtin_offsetof(XcBgftParamEx, slot) == 104) ? 1 : -1];
typedef char xc_bgft_err_check[(__builtin_offsetof(XcBgftProgPrefix, error_result) == 4) ? 1 : -1];
typedef char xc_bgft_len_check[(__builtin_offsetof(XcBgftProgPrefix, length) == 8) ? 1 : -1];
typedef char xc_bgft_got_check[(__builtin_offsetof(XcBgftProgPrefix, transferred) == 16) ? 1 : -1];

/* 0 e «già inizializzato» sono entrambi un init usabile. */
static int xc_user_init_ok(int rc) {
    return rc == 0 || (unsigned)rc == XC_USER_ALREADY_INIT;
}

/* Primo piano, poi utente iniziale, poi il primo id di login > 0. */
static int xc_pick_user(int fg_rc, int fg_id, int init_rc, int init_id, const int* login, int nlogin) {
    int i;
    if (fg_rc == 0 && fg_id > 0) return fg_id;
    if (init_rc == 0 && init_id > 0) return init_id;
    if (!login || nlogin < 0) nlogin = 0;
    for (i = 0; i < nlogin; i++) {
        if (login[i] > 0) return login[i];
    }
    return -1;
}

static void xc_fill_bgft(XcBgftParamEx* ex, int user, const char* url, unsigned long size) {
    ex->param.user_id = user;
    ex->param.entitlement_type = 5;
    ex->param.id = "";
    ex->param.content_url = url;
    ex->param.content_ex_url = "";
    ex->param.content_name = "PSCloud";
    ex->param.icon_path = "/update/fakepic.png";
    ex->param.sku_id = "";
    ex->param.option = XC_OPT_DISABLE_CDN | XC_OPT_FORCE_UPDATE;
    ex->param.playgo_scenario_id = "0";
    ex->param.release_date = "";
    ex->param.package_type = "";
    ex->param.package_sub_type = "";
    ex->param.package_size = size;
    ex->slot = 0;
}

/* Stesse regole del server finto: ciò che la 01.48 ha mandato torna 80990004. */
static int xc_bgft_register_rc(const XcBgftParamEx* ex) {
    const XcBgftParam* p;
    if (!ex) return (int)XC_BGFT_INVALID;
    p = &ex->param;
    if (p->user_id <= 0) return (int)XC_BGFT_INVALID;
    if (!p->id || !p->content_url || !p->content_ex_url || !p->content_name || !p->icon_path || !p->sku_id ||
        !p->playgo_scenario_id || !p->release_date || !p->package_type || !p->package_sub_type)
        return (int)XC_BGFT_INVALID;
    if (p->id[0] != 0) return (int)XC_BGFT_INVALID;
    if (p->content_url[0] == 0 || p->content_name[0] == 0 || p->icon_path[0] == 0) return (int)XC_BGFT_INVALID;
    if (p->playgo_scenario_id[0] != '0' || p->playgo_scenario_id[1] != 0) return (int)XC_BGFT_INVALID;
    if (p->package_size == 0) return (int)XC_BGFT_INVALID;
    if ((p->option & XC_OPT_DISABLE_CDN) == 0) return (int)XC_BGFT_INVALID;
    if (p->entitlement_type != 5) return (int)XC_BGFT_INVALID;
    return 0;
}

typedef struct XcInstallState {
    int step;
    int task;
    int user;
    int last_rc;
    int error_result;
    int prepare_done;
    unsigned long length;
    unsigned long transferred;
    XcBgftParamEx view;
} XcInstallState;

static void xc_install_begin(XcInstallState* s, int user, const char* path, unsigned long size) {
    unsigned char* b = (unsigned char*)s;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    s->task = -1;
    s->user = user;
    if (user <= 0) {
        s->step = XC_STEP_NO_USER;
        s->last_rc = XC_DRIVE_NO_USER;
        return;
    }
    if (!path || !path[0] || size == 0) {
        s->step = XC_STEP_FAIL;
        s->last_rc = XC_DRIVE_BAD_ARG;
        return;
    }
    xc_fill_bgft(&s->view, user, path, size);
    s->last_rc = xc_bgft_register_rc(&s->view);
    s->step = s->last_rc == 0 ? XC_STEP_REGISTER : XC_STEP_FAIL;
}

static void xc_install_on_register(XcInstallState* s, int rc, int task) {
    s->last_rc = rc;
    s->task = task;
    if (rc == 0 && task >= 0) {
        s->step = XC_STEP_START;
        return;
    }
    if ((unsigned)rc == XC_BGFT_BUSY && task >= 0) {
        s->step = XC_STEP_POLL;
        return;
    }
    if (!s->prepare_done && ((unsigned)rc == XC_BGFT_ALREADY || (unsigned)rc == XC_BGFT_EXISTS)) {
        s->step = XC_STEP_PREPARE;
        return;
    }
    s->step = XC_STEP_FAIL;
}

static void xc_install_on_prepare(XcInstallState* s, int rc) {
    (void)rc;
    s->prepare_done = 1;
    s->view.param.option |= XC_OPT_FORCE_UPDATE;
    s->step = XC_STEP_REGISTER;
}

static void xc_install_on_start(XcInstallState* s, int rc) {
    s->last_rc = rc;
    s->step = rc == 0 ? XC_STEP_POLL : XC_STEP_FAIL;
}

static void xc_install_on_progress(XcInstallState* s, int pr, int error_result, unsigned long length,
                                   unsigned long transferred) {
    (void)pr;
    s->error_result = error_result;
    s->length = length;
    s->transferred = transferred;
    if (error_result != 0) {
        s->last_rc = error_result;
        s->step = XC_STEP_FAIL;
        return;
    }
    if (length > 0 && transferred >= length) {
        s->last_rc = 0;
        s->step = XC_STEP_DONE;
        return;
    }
    s->step = XC_STEP_POLL;
}

typedef struct XcDriveHooks {
    int (*reg)(void* ctx, const XcBgftParamEx* ex, int* task);
    int (*start)(void* ctx, int task);
    int (*prog)(void* ctx, int task, int* error_result, unsigned long* length, unsigned long* transferred);
    int (*prep)(void* ctx, const char* path);
    void (*sleep_ms)(void* ctx, int ms);
    void* ctx;
    int polls;
    int max_polls;
} XcDriveHooks;

/* Un solo tentativo di PrepareOverwrite, poi di nuovo il register.
 * Non chiama AppInstallPkg e non disinstalla. */
static int xc_drive_install(XcDriveHooks* h, int user, const char* path, unsigned long size) {
    XcInstallState s;
    int guard = 0;
    if (!h || user <= 0) return XC_DRIVE_NO_USER;
    xc_install_begin(&s, user, path, size);
    if (s.step == XC_STEP_NO_USER) return XC_DRIVE_NO_USER;
    if (s.step == XC_STEP_FAIL) return s.last_rc ? s.last_rc : -1;
    while (s.step == XC_STEP_REGISTER || s.step == XC_STEP_PREPARE || s.step == XC_STEP_START || s.step == XC_STEP_POLL) {
        if (++guard > 100000) return XC_DRIVE_TIMEOUT;
        if (s.step == XC_STEP_REGISTER) {
            int task = -1;
            int rc = h->reg ? h->reg(h->ctx, &s.view, &task) : -1;
            xc_install_on_register(&s, rc, task);
        } else if (s.step == XC_STEP_PREPARE) {
            int rc = h->prep ? h->prep(h->ctx, path) : -1;
            xc_install_on_prepare(&s, rc);
        } else if (s.step == XC_STEP_START) {
            int rc = h->start ? h->start(h->ctx, s.task) : -1;
            xc_install_on_start(&s, rc);
            if (s.step == XC_STEP_FAIL) return XC_DRIVE_STARTED_FAIL;
        } else {
            int err = 0;
            unsigned long len = 0, got = 0;
            if (h->prog) h->prog(h->ctx, s.task, &err, &len, &got);
            xc_install_on_progress(&s, 0, err, len, got);
            if (s.step == XC_STEP_FAIL) return XC_DRIVE_STARTED_FAIL;
            if (s.step == XC_STEP_POLL) {
                h->polls++;
                if (h->max_polls > 0 && h->polls >= h->max_polls) return XC_DRIVE_TIMEOUT;
                if (h->sleep_ms) h->sleep_ms(h->ctx, 200);
            }
        }
    }
    if (s.step == XC_STEP_DONE) return XC_DRIVE_OK;
    return s.last_rc ? s.last_rc : -1;
}

#endif
