/* SPDX-License-Identifier: GPL-3.0-or-later
 * Payload GoldHEN (BinLoader TCP 9090), firmware 11.52.
 *
 * Shellcode PIC: il .bin inizia con _start, niente ELF. Gira nel processo
 * di GoldHEN, fuori dalla sandbox.
 *
 * 01.46 si fermava su «apro i moduli»: load_mod chiamava la syscall 594
 * (dynlib_load_prx) prima di sceKernelLoadStartModule, e _start tornava
 * (rientro, log doppio). Qui:
 *   - lock O_EXCL, un solo payload lavora
 *   - niente syscall 594
 *   - solo sceKernelLoadStartModule (dlsym, syscall 591)
 *   - buffer di notifica statico (niente 0xC30 sullo stack del binloader)
 *   - attesa che XCLD00001 non sia più in esecuzione
 *   - BGFT con l'utente in primo piano e i parametri di Itemzflow
 *   - _start non torna: nanosleep per sempre
 */
#include "install_plan.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef int i32;
typedef long i64;

#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_unlink 10
#define SYS_fsync 95
#define SYS_mkdir 136
#define SYS_nanosleep 240
#define SYS_getdents 272
#define SYS_mmap 477
#define SYS_dynlib_dlsym 591

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x200
#define O_EXCL 0x800
#define O_APPEND 8

#define PROT_RW 3
#define MAP_PRIVATE 0x2
#define MAP_ANON 0x1000

#define TITLE_ID "XCLD00001"
#define CONTENT_ID "IV0000-XCLD00001_00-PSBOXCLOUD000001"
#define PKG_DIR "/data/pkg"
#define DATA_DIR "/data/psbox"
#define LOG_PATH "/data/psbox/payload.log"
#define LOCK_PATH "/data/psbox/payload.lock"

#define NOTE_SIZE 0xC30
#define NOTE_MSG 0x2d

typedef i32 (*fn_dlsym)(i32, const char*, void*);
typedef i32 (*fn_load)(const char*, u64, const void*, u32, void*, i32*);
typedef i32 (*fn_notify)(i32, void*, u64, i32);
typedef i32 (*fn_void)(void);
typedef i32 (*fn_i32)(i32);
typedef i32 (*fn_exists)(const char*, i32*);
typedef i32 (*fn_install)(const char*, void*);
typedef i32 (*fn_initp)(void*);
typedef i32 (*fn_reg)(void*, i32*);
typedef i32 (*fn_prog)(i32, void*);
typedef i32 (*fn_appid)(const char*);
typedef i32 (*fn_user_init)(void*);
typedef i32 (*fn_user)(i32*);
typedef i32 (*fn_user_list)(void*);
typedef i32 (*fn_prep)(const char*);
typedef i32 (*fn_usleep)(u32);
typedef i64 (*fn_lseek)(i32, i64, i32);

typedef struct {
    i32 priority;
} UserInitParam;

typedef struct {
    void* heap;
    u64 heapSize;
    i32 priority;
    i32 pad;
} BgftInit;

static u8 g_note[NOTE_SIZE];

static i64 sc(i64 num, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    i64 ret;
    u8 cf;
    register i64 r10 asm("r10") = d;
    register i64 r8 asm("r8") = e;
    register i64 r9 asm("r9") = f;
    asm volatile("syscall\n\tsetc %1"
                 : "=a"(ret), "=qm"(cf)
                 : "a"(num), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    if (cf) return -ret;
    return ret;
}

static void zmem(void* p, u64 n) {
    u8* b = p;
    while (n--) *b++ = 0;
}

static u64 slen(const char* s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void scopy(char* d, const char* s, u64 cap) {
    u64 i = 0;
    if (!cap) return;
    while (s[i] && i + 1 < cap) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static int cmpstr(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

static int starts(const char* s, const char* p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static int ends_pkg(const char* s) {
    u64 n = slen(s);
    return n > 4 && s[n - 4] == '.' && s[n - 3] == 'p' && s[n - 2] == 'k' && s[n - 1] == 'g';
}

static void hex8(char* d, u32 v) {
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--) d[i] = h[(v >> ((7 - i) * 4)) & 0xf];
    d[8] = 0;
}

static int kdlsym(i32 handle, const char* name, void* out) {
    *(void**)out = 0;
    return (int)sc(SYS_dynlib_dlsym, handle, (i64)name, (i64)out, 0, 0, 0);
}

static int resolve(const char* name, void* out) {
    i32 handles[3];
    u64 i;
    handles[0] = 0x2001;
    handles[1] = 0x1;
    handles[2] = 0x2002;
    for (i = 0; i < 3; i++) {
        if (kdlsym(handles[i], name, out) == 0 && *(void**)out) return 0;
    }
    return -1;
}

static void plog(const char* text) {
    char line[400];
    u64 n = 0;
    i64 fd;
    if (!text) return;
    while (text[n] && n + 2 < sizeof line) {
        line[n] = text[n];
        n++;
    }
    line[n++] = '\n';
    fd = sc(SYS_open, (i64)LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666, 0, 0, 0);
    if (fd < 0) return;
    sc(SYS_write, fd, (i64)line, (i64)n, 0, 0, 0);
    sc(SYS_fsync, fd, 0, 0, 0, 0, 0);
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
}

static void notify(fn_notify fn, const char* text) {
    u64 i;
    if (!fn || !text) return;
    zmem(g_note, sizeof g_note);
    *(u32*)(g_note + 0) = NOTE_SIZE;
    for (i = 0; text[i] && i < 190; i++) g_note[NOTE_MSG + i] = (u8)text[i];
    fn(0, g_note, NOTE_SIZE, 0);
}

static void step(fn_notify fn, const char* text) {
    notify(fn, text);
    plog(text);
}

static void step_rc(fn_notify fn, const char* text, i64 rc) {
    char msg[180];
    char hex[9];
    u64 i = 0;
    u64 j = 0;
    hex8(hex, (u32)rc);
    while (text[i] && i + 12 < sizeof msg) {
        msg[i] = text[i];
        i++;
    }
    msg[i++] = ' ';
    while (hex[j] && i + 1 < sizeof msg) msg[i++] = hex[j++];
    msg[i] = 0;
    step(fn, msg);
}

static void pause_ms(fn_usleep usleep, u32 ms) {
    struct {
        i64 sec;
        i64 nsec;
    } ts;
    if (usleep) {
        usleep(ms * 1000);
        return;
    }
    ts.sec = ms / 1000;
    ts.nsec = (i64)(ms % 1000) * 1000000L;
    sc(SYS_nanosleep, (i64)&ts, 0, 0, 0, 0, 0);
}

static void park(fn_usleep usleep) {
    for (;;) pause_ms(usleep, 60000);
}

static i32 load_sprx(fn_notify note, fn_load load, const char* path) {
    i32 handle;
    if (!load) {
        step(note, "PSBox: sceKernelLoadStartModule assente");
        return -1;
    }
    handle = load(path, 0, 0, 0, 0, 0);
    step_rc(note, path, handle);
    return handle;
}

static i32 load_one(fn_notify note, fn_load load, const char* a, const char* b) {
    i32 h = load_sprx(note, load, a);
    if (h > 0) return h;
    return load_sprx(note, load, b);
}

static int newest_pkg(fn_notify note, char* out, u64 outlen) {
    i64 fd = sc(SYS_open, (i64)PKG_DIR, O_RDONLY, 0, 0, 0, 0);
    u8 buf[2048];
    char best[96];
    best[0] = 0;
    if (fd < 0) {
        step_rc(note, "PSBox: apro /data/pkg", fd);
        return -1;
    }
    for (;;) {
        i64 n = sc(SYS_getdents, fd, (i64)buf, (i64)sizeof buf, 0, 0, 0);
        i64 off = 0;
        if (n <= 0) break;
        while (off + 8 <= n) {
            u16 reclen = *(u16*)(buf + off + 4);
            u8 namlen = buf[off + 7];
            char name[96];
            u64 i;
            if (reclen < 8 || off + reclen > n) break;
            if (namlen >= sizeof name) namlen = sizeof name - 1;
            for (i = 0; i < namlen; i++) name[i] = (char)buf[off + 8 + i];
            name[namlen] = 0;
            if (starts(name, "PSBox-PS4-") && ends_pkg(name)) {
                plog(name);
                if (!best[0] || cmpstr(name, best) > 0) scopy(best, name, sizeof best);
            }
            off += reclen;
        }
    }
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    if (!best[0]) return -1;
    if (slen(PKG_DIR) + 1 + slen(best) + 1 > outlen) return -1;
    scopy(out, PKG_DIR, outlen);
    {
        u64 n = slen(out);
        out[n++] = '/';
        scopy(out + n, best, outlen - n);
    }
    return 0;
}

static void ver_from_path(const char* path, char* out, u64 cap) {
    const char* slash = path;
    const char* p = path;
    const char* dot = 0;
    const char* dash = 0;
    while (*p) {
        if (*p == '/') slash = p + 1;
        p++;
    }
    p = slash;
    while (*p) {
        if (*p == '-') dash = p + 1;
        if (*p == '.') dot = p;
        p++;
    }
    if (!dash || !dot || dot <= dash) {
        scopy(out, "??", cap);
        return;
    }
    {
        u64 n = 0;
        while (dash + n < dot && n + 1 < cap) {
            out[n] = dash[n];
            n++;
        }
        out[n] = 0;
    }
}

static u64 file_size(fn_lseek lseek_fn, const char* path) {
    i64 fd;
    i64 n;
    if (!lseek_fn) return 0;
    fd = sc(SYS_open, (i64)path, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return 0;
    n = lseek_fn((i32)fd, 0, 2);
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    if (n < 0) return 0;
    return (u64)n;
}

typedef struct {
    fn_reg reg;
    fn_i32 start;
    fn_i32 start_int;
    fn_prog prog;
    fn_prep prep;
    fn_usleep usleep;
    fn_notify note;
    int logged;
} PayCtx;

static i32 load_i32(const u8* p) {
    return (i32)((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24));
}

static u64 load_u64(const u8* p) {
    u64 v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= (u64)p[i] << (8 * i);
    return v;
}

static int pay_reg(void* ctx, const XcBgftParamEx* ex, int* task) {
    PayCtx* c = (PayCtx*)ctx;
    i32 id = -1;
    i32 rc;
    *task = -1;
    if (!c->reg || !ex) return -1;
    rc = c->reg((void*)ex, &id);
    *task = id;
    step_rc(c->note, "PSBox: BGFT register", rc);
    return rc;
}

static int pay_start(void* ctx, int task) {
    PayCtx* c = (PayCtx*)ctx;
    i32 rc = -1;
    if (c->start) {
        rc = c->start(task);
        step_rc(c->note, "PSBox: BGFT start", rc);
    }
    if (rc != 0 && c->start_int) {
        rc = c->start_int(task);
        step_rc(c->note, "PSBox: BGFT start int", rc);
    }
    return rc;
}

static int pay_prog(void* ctx, int task, int* error_result, unsigned long* length, unsigned long* transferred) {
    PayCtx* c = (PayCtx*)ctx;
    u8 raw[128];
    i32 pr;
    zmem(raw, sizeof raw);
    *error_result = 0;
    *length = 0;
    *transferred = 0;
    if (!c->prog) return -1;
    pr = c->prog(task, raw);
    *error_result = load_i32(raw + 4);
    *length = load_u64(raw + 8);
    *transferred = load_u64(raw + 16);
    c->logged++;
    if ((c->logged % 4) == 1) step_rc(c->note, "PSBox: BGFT progress", pr);
    if (*error_result != 0) step_rc(c->note, "PSBox: BGFT errore task", *error_result);
    return pr;
}

static int pay_prep(void* ctx, const char* path) {
    PayCtx* c = (PayCtx*)ctx;
    i32 rc;
    if (!c->prep) {
        step(c->note, "PSBox: PrepareOverwrite assente, ritento il register");
        return -1;
    }
    rc = c->prep(path);
    step_rc(c->note, "PSBox: PrepareOverwrite", rc);
    return rc;
}

static void pay_sleep(void* ctx, int ms) {
    PayCtx* c = (PayCtx*)ctx;
    if (ms < 0) ms = 0;
    pause_ms(c->usleep, (u32)ms);
}

static int bgft_install(fn_notify note, i32 bgft, fn_void app_init, fn_prep prep, fn_usleep usleep, const char* pkg,
                        u64 size, i32 user_id) {
    fn_initp bgft_init = 0;
    fn_initp bgft_init_int = 0;
    fn_reg reg = 0;
    fn_i32 start = 0;
    fn_i32 start_int = 0;
    fn_prog prog = 0;
    BgftInit ip;
    PayCtx ctx;
    XcDriveHooks hooks;
    void* heap;
    i32 rc;
    if (user_id <= 0) {
        step(note, "PSBox: utente assente, BGFT non chiamato");
        return XC_DRIVE_NO_USER;
    }
    kdlsym(bgft, "sceBgftServiceInit", &bgft_init);
    kdlsym(bgft, "sceBgftServiceIntInit", &bgft_init_int);
    kdlsym(bgft, "sceBgftServiceIntDownloadRegisterTaskByStorageEx", &reg);
    kdlsym(bgft, "sceBgftServiceDownloadStartTask", &start);
    kdlsym(bgft, "sceBgftServiceIntDownloadStartTask", &start_int);
    kdlsym(bgft, "sceBgftServiceDownloadGetProgress", &prog);
    if (!reg || (!bgft_init && !bgft_init_int)) {
        step(note, "PSBox: simboli BGFT assenti");
        return -1;
    }
    if (app_init) step_rc(note, "PSBox: AppInst init", app_init());
    heap = (void*)sc(SYS_mmap, 0, 1 << 20, PROT_RW, MAP_PRIVATE | MAP_ANON, -1, 0);
    step_rc(note, "PSBox: heap BGFT", (i64)heap);
    if ((u64)heap > 0xfffffffffffff000ul) {
        step(note, "PSBox: mmap heap fallita");
        return -1;
    }
    zmem(&ip, sizeof ip);
    ip.heap = heap;
    ip.heapSize = 1 << 20;
    ip.priority = 700;
    rc = -1;
    if (bgft_init) rc = bgft_init(&ip);
    step_rc(note, "PSBox: BGFT init", rc);
    /* 0x80990001 = già inizializzato, come 0. */
    if (rc != 0 && (u32)rc != 0x80990001u && bgft_init_int) {
        rc = bgft_init_int(&ip);
        step_rc(note, "PSBox: BGFT int init", rc);
    }
    if (rc != 0 && (u32)rc != 0x80990001u) {
        step(note, "PSBox: BGFT init rifiutato");
        return -1;
    }
    zmem(&ctx, sizeof ctx);
    ctx.reg = reg;
    ctx.start = start;
    ctx.start_int = start_int;
    ctx.prog = prog;
    ctx.prep = prep;
    ctx.usleep = usleep;
    ctx.note = note;
    zmem(&hooks, sizeof hooks);
    hooks.reg = pay_reg;
    hooks.start = pay_start;
    hooks.prog = pay_prog;
    hooks.prep = pay_prep;
    hooks.sleep_ms = pay_sleep;
    hooks.ctx = &ctx;
    hooks.max_polls = 600;
    plog(pkg);
    rc = xc_drive_install(&hooks, user_id, pkg, (unsigned long)size);
    if (rc == 0) step(note, "PSBox: BGFT completo");
    return rc;
}

__attribute__((section(".text.start"), used)) void _start(u64 arg) {
    fn_notify notify_fn = 0;
    fn_load load = 0;
    fn_void app_init = 0;
    fn_exists exists = 0;
    fn_prep prep = 0;
    fn_appid get_app = 0;
    fn_user_init user_init = 0;
    fn_user get_fg = 0;
    fn_user get_ini = 0;
    fn_user_list get_list = 0;
    fn_usleep usleep = 0;
    fn_lseek lseek_fn = 0;
    i32 appinst = -1;
    i32 bgft = -1;
    i32 lnc = -1;
    i32 user = -1;
    i32 user_id = -1;
    i32 fg_id = -1;
    i32 ini_id = -1;
    i32 fg_rc = -1;
    i32 ini_rc = -1;
    i32 list_rc = -1;
    i32 login_id[4];
    i32 rc;
    i64 lockfd;
    char pkg[160];
    char url_user[192];
    char ver[16];
    char done[64];
    u64 size;
    int i;
    int gone;

    if (resolve("sceKernelSendNotificationRequest", &notify_fn) != 0 && arg > 0x10000 && (arg & 7) == 0) {
        fn_dlsym dl = *(fn_dlsym*)arg;
        u64 raw = (u64)dl;
        if (raw > 0x10000 && (raw & 7) == 0) dl(0x2001, "sceKernelSendNotificationRequest", &notify_fn);
    }
    notify(notify_fn, "PSBox payload avviato");
    sc(SYS_mkdir, (i64)DATA_DIR, 0777, 0, 0, 0, 0);
    plog("payload PSBox: niente syscall 594, solo LoadStartModule");

    lockfd = sc(SYS_open, (i64)LOCK_PATH, O_WRONLY | O_CREAT | O_EXCL, 0666, 0, 0, 0);
    if (lockfd < 0) {
        plog("payload già in esecuzione, questo avvio si ferma");
        park(0);
    }
    sc(SYS_write, lockfd, (i64) "1\n", 2, 0, 0, 0);
    sc(SYS_close, lockfd, 0, 0, 0, 0, 0);

    resolve("sceKernelLoadStartModule", &load);
    resolve("sceKernelUsleep", &usleep);
    resolve("sceKernelLseek", &lseek_fn);

    /* L'app sta uscendo. I moduli si caricano dopo, non con la syscall 594. */
    step(notify_fn, "PSBox: attendo l'uscita prima dei moduli");
    pause_ms(usleep, 3000);

    step(notify_fn, "PSBox: apro i moduli");
    appinst = load_one(notify_fn, load, "/system/common/lib/libSceAppInstUtil.sprx",
                       "/system/priv/lib/libSceAppInstUtil.sprx");
    bgft = load_one(notify_fn, load, "/system/priv/lib/libSceBgft.sprx", "/system/common/lib/libSceBgft.sprx");
    lnc = load_one(notify_fn, load, "/system/common/lib/libSceLncUtil.sprx", "/system/priv/lib/libSceLncUtil.sprx");
    step_rc(notify_fn, "PSBox: AppInst", appinst);
    step_rc(notify_fn, "PSBox: BGFT", bgft);
    step_rc(notify_fn, "PSBox: Lnc", lnc);
    step(notify_fn, "PSBox: moduli aperti");

    if (appinst > 0) {
        kdlsym(appinst, "sceAppInstUtilInitialize", &app_init);
        kdlsym(appinst, "sceAppInstUtilAppExists", &exists);
        kdlsym(appinst, "sceAppInstUtilAppPrepareOverwritePkg", &prep);
    }
    if (lnc > 0) kdlsym(lnc, "sceLncUtilGetAppId", &get_app);

    gone = 0;
    step(notify_fn, "PSBox: attendo che XCLD00001 non sia in esecuzione");
    for (i = 0; i < 45; i++) {
        i32 id = get_app ? get_app(TITLE_ID) : -1;
        if ((i % 5) == 0) step_rc(notify_fn, "PSBox: app id", id);
        if (!get_app) {
            pause_ms(usleep, 8000);
            gone = 1;
            break;
        }
        if (id < 0) {
            gone = 1;
            break;
        }
        pause_ms(usleep, 1000);
    }
    if (!gone) step(notify_fn, "PSBox: l'app risulta ancora aperta, installo lo stesso");

    if (newest_pkg(notify_fn, pkg, sizeof pkg) != 0) {
        step(notify_fn, "PSBox: pkg assente in /data/pkg");
        sc(SYS_unlink, (i64)LOCK_PATH, 0, 0, 0, 0, 0);
        park(usleep);
    }
    plog(pkg);
    ver_from_path(pkg, ver, sizeof ver);
    size = file_size(lseek_fn, pkg);
    step_rc(notify_fn, "PSBox: size pkg", (i64)size);

    user = load_one(notify_fn, load, "/system/common/lib/libSceUserService.sprx",
                    "/system/priv/lib/libSceUserService.sprx");
    if (user > 0) {
        kdlsym(user, "sceUserServiceInitialize", &user_init);
        kdlsym(user, "sceUserServiceGetForegroundUser", &get_fg);
        kdlsym(user, "sceUserServiceGetInitialUser", &get_ini);
        kdlsym(user, "sceUserServiceGetLoginUserIdList", &get_list);
    }
    if (user_init) {
        UserInitParam up;
        up.priority = 700;
        rc = user_init(&up);
        step_rc(notify_fn, "PSBox: user init", rc);
        /* 0x80960003 = già inizializzato: si legge lo stesso l'utente. */
        (void)xc_user_init_ok(rc);
    }
    zmem(login_id, sizeof login_id);
    if (get_fg) {
        fg_rc = get_fg(&fg_id);
        step_rc(notify_fn, "PSBox: foreground rc", fg_rc);
        step_rc(notify_fn, "PSBox: foreground", fg_id);
    }
    if (get_ini) {
        ini_rc = get_ini(&ini_id);
        step_rc(notify_fn, "PSBox: initial rc", ini_rc);
        step_rc(notify_fn, "PSBox: initial", ini_id);
    }
    if (get_list) {
        list_rc = get_list(login_id);
        step_rc(notify_fn, "PSBox: login rc", list_rc);
    }
    user_id = xc_pick_user(fg_rc, fg_id, ini_rc, ini_id, login_id, 4);
    step_rc(notify_fn, "PSBox: user", user_id);

    rc = -1;
    if (bgft > 0 && user_id > 0) {
        rc = bgft_install(notify_fn, bgft, app_init, prep, usleep, pkg, size, user_id);
        if (rc != 0 && rc != XC_DRIVE_NO_USER && rc != XC_DRIVE_STARTED_FAIL && rc != XC_DRIVE_TIMEOUT) {
            scopy(url_user, "/user", sizeof url_user);
            {
                u64 n = slen(url_user);
                scopy(url_user + n, pkg, sizeof url_user - n);
            }
            step(notify_fn, "PSBox: ritento BGFT sul path /user");
            rc = bgft_install(notify_fn, bgft, app_init, prep, usleep, url_user, size, user_id);
        }
    }
    if (rc != 0) {
        if (rc == XC_DRIVE_TIMEOUT) step(notify_fn, "PSBox: BGFT timeout, pkg lasciato in /data/pkg");
        else step(notify_fn, "PSBox: BGFT non installato, pkg lasciato in /data/pkg");
        sc(SYS_unlink, (i64)LOCK_PATH, 0, 0, 0, 0, 0);
        park(usleep);
    }

    scopy(done, "PSCloud aggiornata a ", sizeof done);
    {
        u64 n = slen(done);
        scopy(done + n, ver, sizeof done - n);
    }
    step(notify_fn, done);
    (void)exists;
    sc(SYS_unlink, (i64)LOCK_PATH, 0, 0, 0, 0, 0);
    park(usleep);
}
