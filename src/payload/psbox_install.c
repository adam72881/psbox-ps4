/* SPDX-License-Identifier: GPL-3.0-or-later
 * Payload GoldHEN per la porta 9090, firmware 11.52.
 *
 * Il BinLoader, se i primi byte sono 7F 45 4C 46, carica un ELF e salta
 * all'entry. Altrimenti copia i byte e salta al primo. L'ELF OpenOrbis
 * precedente aveva il crt e i PLT verso libkernel non risolti: moriva prima
 * di main, e il .bin era la stessa copia. Questo file è shellcode PIC:
 * il .bin inizia con _start, senza header ELF.
 *
 * Gira nel processo di GoldHEN, già fuori dalla sandbox. Non c'è un secondo
 * escape: se una syscall torna un errore, la notifica e il log lo dicono.
 * I simboli arrivano da syscall 591 (sys_dynlib_dlsym), handle 0x2001.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef int i32;
typedef long i64;

#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_fsync 95
#define SYS_mkdir 136
#define SYS_nanosleep 240
#define SYS_getdents 272
#define SYS_dynlib_dlsym 591
#define SYS_dynlib_load_prx 594

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x200
#define O_TRUNC 0x400
#define O_APPEND 8

#define TITLE_ID "XCLD00001"
#define CONTENT_ID "IV0000-XCLD00001_00-PSBOXCLOUD000001"
#define PKG_DIR "/data/pkg"
#define DATA_DIR "/data/psbox"
#define LOG_PATH "/data/psbox/payload.log"
#define AUTOSTART_PATH "/data/psbox/autostart.txt"

#define NOTE_SIZE 0xC30
#define NOTE_MSG 0x2d

typedef i32 (*fn_dlsym)(i32, const char*, void*);
typedef i32 (*fn_load)(const char*, u64, const void*, u32, void*, i32*);
typedef i32 (*fn_sysmod)(u32);
typedef i32 (*fn_notify)(i32, void*, u64, i32);
typedef i32 (*fn_void)(void);
typedef i32 (*fn_exists)(const char*, i32*);
typedef i32 (*fn_title)(const char*);
typedef i32 (*fn_install)(const char*, void*);
typedef i32 (*fn_progress)(const char*, u32*);
typedef i32 (*fn_user_init)(void*);
typedef i32 (*fn_user)(i32*);
typedef i32 (*fn_usleep)(u32);

typedef struct {
    u32 size;
    u32 user_id;
    u32 app_opt;
    u64 crash_report;
    u32 flag;
} LncParam;

typedef i32 (*fn_launch)(const char*, const char*[], LncParam*);

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

static int scmp(const char* a, const char* b) {
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
    for (int i = 7; i >= 0; i--) d[i] = h[(v >> ((7 - i) * 4)) & 0xf];
    d[8] = 0;
}

static int kdlsym(i32 handle, const char* name, void* out) {
    *(void**)out = 0;
    return (int)sc(SYS_dynlib_dlsym, handle, (i64)name, (i64)out, 0, 0, 0);
}

static int resolve(const char* name, void* out) {
    static const i32 handles[] = {0x2001, 0x1, 0x2002};
    for (u64 i = 0; i < sizeof handles / sizeof handles[0]; i++) {
        if (kdlsym(handles[i], name, out) == 0 && *(void**)out) return 0;
    }
    return -1;
}

static void notify(fn_notify fn, const char* text) {
    u8 req[NOTE_SIZE];
    u64 i;
    if (!fn || !text) return;
    zmem(req, sizeof req);
    *(u32*)(req + 0) = NOTE_SIZE;
    for (i = 0; text[i] && i < 190; i++) req[NOTE_MSG + i] = (u8)text[i];
    fn(0, req, NOTE_SIZE, 0);
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

static void step(fn_notify fn, const char* text) {
    notify(fn, text);
    plog(text);
}

static void step_rc(fn_notify fn, const char* text, i64 rc) {
    char msg[160];
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
    if (usleep) {
        usleep(ms * 1000);
        return;
    }
    struct {
        i64 sec;
        i64 nsec;
    } ts;
    ts.sec = ms / 1000;
    ts.nsec = (i64)(ms % 1000) * 1000000L;
    sc(SYS_nanosleep, (i64)&ts, 0, 0, 0, 0, 0);
}

static i32 load_mod(fn_notify note, fn_load load, fn_sysmod sysmod, const char* name, const char* path, u32 sysid) {
    i32 handle = -1;
    i64 rc = sc(SYS_dynlib_load_prx, (i64)name, 0, 0, 0, 0, (i64)&handle);
    if (rc != 0 || handle < 0) {
        handle = -1;
        rc = sc(SYS_dynlib_load_prx, (i64)path, 0, 0, 0, 0, (i64)&handle);
    }
    if ((rc != 0 || handle < 0) && load) {
        handle = load(name, 0, 0, 0, 0, 0);
        if (handle < 0) handle = load(path, 0, 0, 0, 0, 0);
    }
    if (handle < 0 && sysmod && sysid) {
        i32 sr = sysmod(sysid);
        step_rc(note, "PSBox: sysmodule", sr);
        handle = -1;
        rc = sc(SYS_dynlib_load_prx, (i64)name, 0, 0, 0, 0, (i64)&handle);
        if (rc != 0 || handle < 0) handle = (i32)rc;
    }
    return handle;
}

static int app_exists(fn_exists exists, const char* title) {
    i32 flag = 0;
    i32 rc = exists(title, &flag);
    if (rc != 0) return -1;
    return flag ? 1 : 0;
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
                if (!best[0] || scmp(name, best) > 0) scopy(best, name, sizeof best);
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

__attribute__((section(".text.start"), used)) void _start(u64 arg) {
    fn_notify notify_fn = 0;
    fn_load load = 0;
    fn_sysmod sysmod = 0;
    fn_void app_init = 0;
    fn_exists exists = 0;
    fn_title uninstall = 0;
    fn_install install = 0;
    fn_progress progress = 0;
    fn_void lnc_init = 0;
    fn_launch launch = 0;
    fn_user_init user_init = 0;
    fn_user user_get = 0;
    fn_usleep usleep = 0;
    i32 appinst;
    i32 lnc;
    char pkg[160];
    i32 rc;
    i32 user_id = 0;
    LncParam param;
    const char* argv[1];

    if (resolve("sceKernelSendNotificationRequest", &notify_fn) != 0 && arg > 0x10000 &&
        (arg & 7) == 0) {
        fn_dlsym dl = *(fn_dlsym*)arg;
        u64 raw = (u64)dl;
        if (raw > 0x10000 && (raw & 7) == 0) dl(0x2001, "sceKernelSendNotificationRequest", &notify_fn);
    }
    /* Prima azione visibile, prima di mkdir e dei moduli. */
    notify(notify_fn, "PSBox payload avviato");

    sc(SYS_mkdir, (i64)DATA_DIR, 0777, 0, 0, 0, 0);
    plog("payload PSBox raw: syscall 591, processo GoldHEN");
    if (!notify_fn) plog("notifica assente");

    resolve("sceKernelLoadStartModule", &load);
    resolve("sceSysmoduleLoadModuleInternal", &sysmod);
    resolve("sceKernelUsleep", &usleep);

    step(notify_fn, "PSBox: apro i moduli");
    appinst = load_mod(notify_fn, load, sysmod, "libSceAppInstUtil.sprx",
                       "/system/common/lib/libSceAppInstUtil.sprx", 0x80000014);
    lnc = load_mod(notify_fn, load, sysmod, "libSceLncUtil.sprx", "/system/common/lib/libSceLncUtil.sprx", 0);
    step_rc(notify_fn, "PSBox: AppInst", appinst);
    step_rc(notify_fn, "PSBox: Lnc", lnc);
    if (appinst < 0 || lnc < 0) {
        step(notify_fn, "PSBox: moduli assenti");
        return;
    }

    kdlsym(appinst, "sceAppInstUtilInitialize", &app_init);
    kdlsym(appinst, "sceAppInstUtilAppExists", &exists);
    kdlsym(appinst, "sceAppInstUtilAppUnInstall", &uninstall);
    kdlsym(appinst, "sceAppInstUtilAppInstallPkg", &install);
    kdlsym(appinst, "sceAppInstUtilGetInstallProgress", &progress);
    kdlsym(lnc, "sceLncUtilInitialize", &lnc_init);
    kdlsym(lnc, "sceLncUtilLaunchApp", &launch);
    if (!app_init || !exists || !uninstall || !install || !launch) {
        step(notify_fn, "PSBox: simboli mancanti");
        return;
    }

    rc = app_init();
    step_rc(notify_fn, "PSBox: init install", rc);
    if (lnc_init) step_rc(notify_fn, "PSBox: init lancio", lnc_init());

    if (app_exists(exists, TITLE_ID) == 1) {
        step(notify_fn, "PSBox: disinstallo");
        rc = uninstall(TITLE_ID);
        step_rc(notify_fn, "PSBox: uninstall", rc);
        {
            int i;
            int gone = 0;
            for (i = 0; i < 60; i++) {
                if (app_exists(exists, TITLE_ID) == 0) {
                    gone = 1;
                    break;
                }
                pause_ms(usleep, 500);
            }
            if (!gone) {
                step(notify_fn, "PSBox: disinstallazione incompleta");
                return;
            }
        }
        step(notify_fn, "PSBox: titolo assente");
    } else {
        step(notify_fn, "PSBox: nessuna versione installata");
    }

    if (newest_pkg(notify_fn, pkg, sizeof pkg) != 0) {
        step(notify_fn, "PSBox: pkg assente in /data/pkg");
        return;
    }
    plog(pkg);
    step(notify_fn, "PSBox: installo il pkg");
    rc = install(pkg, 0);
    step_rc(notify_fn, "PSBox: install", rc);
    if (rc != 0) {
        step(notify_fn, "PSBox: installazione rifiutata");
        return;
    }
    {
        int i;
        int saw = 0;
        int ok = 0;
        for (i = 0; i < 180; i++) {
            u32 pct = 0;
            i32 pr = progress ? progress(CONTENT_ID, &pct) : -1;
            int there = app_exists(exists, TITLE_ID);
            if (pr == 0 && pct > 0 && pct < 100) saw = 1;
            if (there == 1 && pr == 0 && pct >= 100) {
                ok = 1;
                break;
            }
            if (there == 1 && !saw && i >= 4 && (pr != 0 || pct == 0 || pct >= 100)) {
                ok = 1;
                break;
            }
            pause_ms(usleep, 500);
        }
        if (!ok) {
            step(notify_fn, "PSBox: installazione non finita");
            return;
        }
    }
    step(notify_fn, "PSBox: pkg installato");

    {
        i64 fd = sc(SYS_open, (i64)AUTOSTART_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666, 0, 0, 0);
        static const char game[] = "Fortnite\n";
        if (fd >= 0) {
            sc(SYS_write, fd, (i64)game, (i64)(sizeof game - 1), 0, 0, 0);
            sc(SYS_fsync, fd, 0, 0, 0, 0, 0);
            sc(SYS_close, fd, 0, 0, 0, 0, 0);
            step(notify_fn, "PSBox: autostart Fortnite");
        } else {
            step_rc(notify_fn, "PSBox: autostart", fd);
        }
    }

    {
        i32 user = load_mod(notify_fn, load, sysmod, "libSceUserService.sprx",
                            "/system/common/lib/libSceUserService.sprx", 0x80000011);
        if (user >= 0) {
            kdlsym(user, "sceUserServiceInitialize", &user_init);
            kdlsym(user, "sceUserServiceGetInitialUser", &user_get);
        }
        if (user_init) user_init(0);
        if (user_get) user_get(&user_id);
    }

    zmem(&param, sizeof param);
    param.size = sizeof param;
    param.user_id = (u32)user_id;
    param.flag = 2; /* salta il controllo dell'aggiornamento di sistema */
    argv[0] = 0;
    step(notify_fn, "PSBox: avvio");
    rc = launch(TITLE_ID, argv, &param);
    step_rc(notify_fn, "PSBox: launch", rc);
    if (rc != 0) step(notify_fn, "PSBox: avvio rifiutato");
    else step(notify_fn, "PSBox: fatto");
}
