// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "app/local_update.h"
#include "app/update_pick.h"
#include "GoldHEN.h"

#include "platform/platform.h"
#include "util/log.h"

#include <orbis/Bgft.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace xc::app {
namespace {

constexpr const char* kUpdateDir = "/data/psbox/update";
constexpr const char* kPkgDir = "/data/pkg";
constexpr const char* kTitleId = "XCLD00001";
constexpr const char* kContentId = "IV0000-XCLD00001_00-PSBOXCLOUD000001";
constexpr const char* kPendingPath = "/data/psbox/update.pending";
constexpr const char* kManualInstall = "Installa a mano da /data/pkg";
// 0x80990001 è il «già inizializzato» classico. 0x80990050 è la sessione
// assente (o l'init rifiutato) visto sulla 01.19: dopo i parametri giusti
// si prosegue comunque con RegisterTask, non si abbandona il pkg.
constexpr uint32_t kBgftAlreadyInit = 0x80990001u;
constexpr uint32_t kBgftNoSession = 0x80990050u;
constexpr uint32_t kBgftAlreadyInstalled = 0x80990088u;
constexpr uint32_t kBgftExists = 0x80990015u;
// Register con userId 0 (GetInitialUser non inizializzato) torna questo.
constexpr uint32_t kBgftInvalidArg = 0x80990002u;
// Task già in corso (Itemzflow 0x80990086): ShellCore sta già installando.
constexpr uint32_t kBgftBusy = 0x80990086u;

// Stamp + fsync prima di una chiamata che può uccidere il processo.
void stampEnter(const char* what) {
    char line[160];
    std::snprintf(line, sizeof line, "update: %s enter", what ? what : "?");
    xc_log_stamp(line);
    XC_LOGI("%s", line);
}

void stampLeave(const char* what, int rc) {
    char line[160];
    std::snprintf(line, sizeof line, "update: %s leave 0x%08x", what ? what : "?", static_cast<unsigned>(rc));
    xc_log_stamp(line);
    XC_LOGI("%s", line);
}

bool readAll(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[1 << 16];
    out.clear();
    while (size_t n = std::fread(buf, 1, sizeof buf, f)) out.append(buf, n);
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad && !out.empty();
}

bool writePending(const std::string& ver) {
    FILE* f = std::fopen(kPendingPath, "w");
    if (!f) return false;
    std::fprintf(f, "%s\n", ver.c_str());
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0) (void)!::fsync(fd);
    std::fclose(f);
    return true;
}

std::string readPending() {
    std::string out;
    if (!readAll(kPendingPath, out)) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

void clearPending() {
    if (unlink(kPendingPath) == 0) XC_LOGI("update: pending rimosso");
}

// param.sfo dentro il pkg: magic \0PSF, chiave APP_VER.
std::string appVerFromPkg(const std::string& bytes) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t n = bytes.size();
    for (size_t i = 0; i + 20 < n; ++i) {
        if (p[i] != 0x00 || p[i + 1] != 0x50 || p[i + 2] != 0x53 || p[i + 3] != 0x46) continue;
        uint32_t keyOff = 0, dataOff = 0, count = 0;
        std::memcpy(&keyOff, p + i + 8, 4);
        std::memcpy(&dataOff, p + i + 12, 4);
        std::memcpy(&count, p + i + 16, 4);
        if (count == 0 || count > 64) continue;
        if (i + 20 + count * 16 > n) continue;
        size_t keys = i + keyOff;
        size_t data = i + dataOff;
        if (keys >= n || data >= n) continue;
        for (uint32_t e = 0; e < count; ++e) {
            const unsigned char* ent = p + i + 20 + e * 16;
            uint16_t krel = 0, fmt = 0;
            uint32_t len = 0, doff = 0;
            std::memcpy(&krel, ent, 2);
            std::memcpy(&fmt, ent + 2, 2);
            std::memcpy(&len, ent + 4, 4);
            std::memcpy(&doff, ent + 12, 4);
            size_t ks = keys + krel;
            if (ks + 8 >= n) continue;
            if (std::strcmp(reinterpret_cast<const char*>(p + ks), "APP_VER") != 0) continue;
            if ((fmt & 0x0400) == 0) continue;  // non è una stringa
            size_t ds = data + doff;
            if (ds >= n || len == 0 || ds + len > n) continue;
            std::string s(reinterpret_cast<const char*>(p + ds), strnlen(reinterpret_cast<const char*>(p + ds), len));
            return s;
        }
    }
    return {};
}

void remember(std::vector<std::string>& out, const std::string& name) {
    if (!isPkgFileName(name)) return;
    for (const auto& have : out)
        if (have == name) return;
    out.push_back(name);
}

// Layout BSD di getdents: fileno a 0, reclen a 4, type a 6, namlen a 7, nome a 8.
void readDents(const unsigned char* buf, int n, std::vector<std::string>& names, std::vector<std::string>& pkgs) {
    int off = 0;
    int seen = 0;
    while (off + 8 <= n && seen < 64) {
        uint16_t reclen = 0;
        std::memcpy(&reclen, buf + off + 4, 2);
        unsigned typ = buf[off + 6];
        unsigned namlen = buf[off + 7];
        if (reclen < 8 || off + reclen > n) {
            XC_LOGW("update: dent rotto off %d reclen %u", off, reclen);
            break;
        }
        if (namlen > 255) namlen = 255;
        if (namlen == 0) {
            const char* s = reinterpret_cast<const char*>(buf + off + 8);
            size_t room = static_cast<size_t>(reclen - 8);
            namlen = static_cast<unsigned>(strnlen(s, room));
        }
        std::string name(reinterpret_cast<const char*>(buf + off + 8), namlen);
        XC_LOGI("update: voce '%s' typ %u len %u", name.c_str(), typ, namlen);
        if (!name.empty() && name != "." && name != "..") names.push_back(name);
        remember(pkgs, name);
        off += reclen;
        ++seen;
    }
}

struct Measured {
    bool exists = false;
    long long lseekBytes = -1;
    long long stat48 = -1;
    unsigned char mag[4] = {};
    bool magic = false;
};

// La dimensione vera è lseek. stat48 legge l'int64 a offset 0x48 del buffer
// del kernel, non st_size di musl (che su Orbis cade sui blocchi, a 0x50).
Measured measurePkg(const std::string& path) {
    Measured m;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return m;
    m.exists = true;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end >= 0) m.lseekBytes = static_cast<long long>(end);
    unsigned char raw[1024];
    std::memset(raw, 0, sizeof raw);
    if (fstat(fd, reinterpret_cast<struct stat*>(raw)) == 0) m.stat48 = orbisStatSize(raw, sizeof raw);
    if (lseek(fd, 0, SEEK_SET) == 0 && read(fd, m.mag, 4) == 4) m.magic = pkgMagicOk(m.mag);
    close(fd);
    return m;
}

std::vector<std::string> listPkgs(const std::string& dir) {
    std::vector<std::string> pkgs;
    int fd = open(dir.c_str(), O_RDONLY);
    if (fd < 0) {
        XC_LOGW("update: open %s errno %d", dir.c_str(), errno);
    } else {
        unsigned char buf[2048];
        for (;;) {
            int n = getdents(fd, reinterpret_cast<dirent*>(buf), sizeof buf);
            if (n < 0) {
                XC_LOGW("update: getdents errno %d", errno);
                break;
            }
            if (n == 0) break;
            XC_LOGI("update: getdents %d byte", n);
            std::vector<std::string> names;
            readDents(buf, n, names, pkgs);
        }
        close(fd);
    }
    int hits = 0;
    for (int n = 0; n <= 99; ++n) {
        char file[64];
        std::snprintf(file, sizeof file, "PSBox-PS4-01.%02d.pkg", n);
        std::string path = dir + "/" + file;
        int pfd = open(path.c_str(), O_RDONLY);
        if (pfd < 0) continue;
        close(pfd);
        ++hits;
        remember(pkgs, file);
    }
    if (hits == 0) XC_LOGI("update: stat diretto nessun PSBox-PS4-01.NN.pkg");
    return pkgs;
}

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    char buf[1 << 16];
    bool ok = true;
    while (size_t n = std::fread(buf, 1, sizeof buf, in)) {
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (std::ferror(in)) ok = false;
    std::fflush(out);
    std::fclose(out);
    std::fclose(in);
    return ok;
}

void unlinkOthers(const std::string& dir, const std::string& keep) {
    for (const auto& name : listPkgs(dir)) {
        if (name == keep) continue;
        std::string path = dir + "/" + name;
        if (unlink(path.c_str()) == 0)
            XC_LOGI("update: rimosso %s", path.c_str());
        else
            XC_LOGW("update: non rimosso %s (%d)", path.c_str(), errno);
    }
}

void clearUpdateDir() {
    for (int n = 0; n <= 99; ++n) {
        char file[64];
        std::snprintf(file, sizeof file, "PSBox-PS4-01.%02d.pkg", n);
        std::string path = std::string(kUpdateDir) + "/" + file;
        if (unlink(path.c_str()) == 0) XC_LOGI("update: rimosso %s", path.c_str());
    }
    int fd = open(kUpdateDir, O_RDONLY);
    if (fd >= 0) {
        unsigned char buf[2048];
        for (;;) {
            int n = getdents(fd, reinterpret_cast<dirent*>(buf), sizeof buf);
            if (n <= 0) break;
            std::vector<std::string> names, pkgs;
            readDents(buf, n, names, pkgs);
            for (const auto& name : names) {
                if (name.empty() || name[0] == '.') continue;
                std::string path = std::string(kUpdateDir) + "/" + name;
                if (unlink(path.c_str()) == 0) XC_LOGI("update: rimosso %s", path.c_str());
            }
        }
        close(fd);
    }
    XC_LOGI("update: cartella %s svuotata", kUpdateDir);
}

// Lo stub PLT di un modulo non mappato uccide il processo (la 01.16 dopo
// 0x80020008). Si chiama solo un puntatore uscito da sceKernelDlsym.
// AppInst sta in /system/priv/lib: common torna ENOENT (0x80020002). Lo
// sysmodule 0x80000014 senza credenziali di sistema torna ENOEXEC (0x80020008).
// BGFT 0x8000002A si carica anche dalla sandbox.
int32_t loadOne(const char* path) {
    int res = 0;
    XC_LOGI("update: LoadStartModule(%s) enter", path);
    int32_t handle = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &res));
    XC_LOGI("update: LoadStartModule(%s) leave handle 0x%08x res 0x%08x", path, static_cast<unsigned>(handle),
            static_cast<unsigned>(res));
    return handle > 0 ? handle : -1;
}

int32_t loadModule(const char* label, const char* const* paths, int npath, uint32_t sysId, const char* probe) {
    for (int i = 0; i < npath; ++i) {
        int32_t handle = loadOne(paths[i]);
        if (handle > 0) return handle;
    }
    if (sysId != 0) {
        XC_LOGI("update: sysmodule %s id 0x%08x enter", label, sysId);
        uint32_t mod = sceSysmoduleLoadModuleInternal(static_cast<OrbisSysModuleInternal>(sysId));
        XC_LOGI("update: sysmodule %s leave 0x%08x", label, mod);
        for (int i = 0; i < npath; ++i) {
            int32_t handle = loadOne(paths[i]);
            if (handle > 0) return handle;
        }
    }
    if (!probe) return -1;
    OrbisKernelModule mods[96];
    size_t avail = 0;
    int list = sceKernelGetModuleList(mods, sizeof mods / sizeof mods[0], &avail);
    XC_LOGI("update: GetModuleList rc 0x%08x count %zu", static_cast<unsigned>(list), avail);
    if (list != 0) return -1;
    size_t n = avail > 96 ? 96 : avail;
    for (size_t i = 0; i < n; ++i) {
        void* sym = nullptr;
        int dr = sceKernelDlsym(static_cast<int32_t>(mods[i]), probe, &sym);
        if (dr == 0 && sym) {
            XC_LOGI("update: %s già in memoria handle 0x%08x", probe, static_cast<unsigned>(mods[i]));
            return static_cast<int32_t>(mods[i]);
        }
    }
    XC_LOGW("update: %s non trovato", probe);
    return -1;
}

// GoldHEN SDK: sys_sdk_jailbreak (libGoldHEN_Hook, syscall 500 comando 2).
// Azzera gli uid, punta rootdir e jaildir alla root vera e mette authid e
// caps di sistema. Senza, /system non si vede (ENOENT) e AppInst è ENOEXEC.
// Niente syscall 11 e niente 594.
bool tryRaiseCreds() {
    jailbreak_backup saved{};
    errno = 0;
    int jr = sys_sdk_jailbreak(&saved);
    XC_LOGI("update: sys_sdk_jailbreak rc %d errno %d uid %u paid 0x%llx", jr, errno, saved.cr_uid,
            static_cast<unsigned long long>(saved.cr_paid));
    int ur = setuid(0);
    int gr = setgid(0);
    XC_LOGI("update: setuid 0 rc %d setgid 0 rc %d", ur, gr);
    int fd = open("/system/common/lib/libSceAppInstUtil.sprx", O_RDONLY);
    XC_LOGI("update: vedo AppInst sprx fd %d errno %d", fd, errno);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    using Jail = int (*)();
    const char* prx[] = {"/data/GoldHEN/jb.prx", "/data/GoldHEN/libjbc.sprx"};
    for (const char* path : prx) {
        int res = 0;
        int32_t h = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &res));
        XC_LOGI("update: LoadStartModule(%s) handle 0x%08x res 0x%08x", path, static_cast<unsigned>(h),
                static_cast<unsigned>(res));
        if (h <= 0) continue;
        void* sym = nullptr;
        int dr = sceKernelDlsym(h, "jailbreak_me", &sym);
        XC_LOGI("update: Dlsym jailbreak_me rc 0x%08x ptr %p", static_cast<unsigned>(dr), sym);
        if (dr != 0 || !sym) continue;
        int j2 = reinterpret_cast<Jail>(sym)();
        XC_LOGI("update: jailbreak_me leave 0x%08x", static_cast<unsigned>(j2));
        return j2 == 0;
    }
    XC_LOGW("update: jailbreak assente, AppInst senza credenziali di sistema");
    return false;
}

// 0x80960002 = UserService non inizializzato. L'update parte prima del pad,
// quindi GetInitialUser da solo lascia userId 0 e BGFT risponde 0x80990002.
int32_t currentUserId() {
    OrbisUserServiceInitializeParams param{700};
    int ir = sceUserServiceInitialize(&param);
    XC_LOGI("update: sceUserServiceInitialize rc 0x%08x", static_cast<unsigned>(ir));
    int32_t user = ORBIS_USER_SERVICE_USER_ID_INVALID;
    int ur = sceUserServiceGetInitialUser(&user);
    XC_LOGI("update: utente iniziale %d rc 0x%08x", user, static_cast<unsigned>(ur));
    if (ur != 0 || user <= 0) {
        int32_t fg = ORBIS_USER_SERVICE_USER_ID_INVALID;
        int fr = sceUserServiceGetForegroundUser(&fg);
        XC_LOGI("update: utente in primo piano %d rc 0x%08x", fg, static_cast<unsigned>(fr));
        if (fr == 0 && fg > 0) user = fg;
    }
    if (user <= 0) {
        XC_LOGW("update: nessun utente, BGFT non va chiamato con user 0");
        return -1;
    }
    return user;
}

template <typename T>
bool resolveSym(int32_t handle, const char* name, T* out) {
    *out = nullptr;
    if (handle <= 0) {
        XC_LOGW("update: Dlsym %s salto, nessun handle", name);
        return false;
    }
    void* addr = nullptr;
    int rc = sceKernelDlsym(handle, name, &addr);
    XC_LOGI("update: Dlsym %s rc 0x%08x ptr %p", name, static_cast<unsigned>(rc), addr);
    if (rc != 0 || !addr) return false;
    *out = reinterpret_cast<T>(addr);
    return true;
}

using AppInit = int32_t (*)();
using AppInstall = int32_t (*)(const char*, void*);

// Come Itemzflow e il payload: solo InstallPkg. Mai PrepareOverwritePkg: sulla
// 01.23 ha disinstallato la versione in esecuzione e l'icona è sparita senza
// installare la nuova. Mai AppUnInstall della versione corrente.
bool installWithAppInst(const char* path) {
    tryRaiseCreds();
    (void)currentUserId();
    const char* paths[] = {"/system/common/lib/libSceAppInstUtil.sprx", "/system/priv/lib/libSceAppInstUtil.sprx"};
    int32_t handle = loadModule("AppInst", paths, 2, ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL, "sceAppInstUtilInitialize");
    AppInit init = nullptr;
    AppInstall inst = nullptr;
    if (!resolveSym(handle, "sceAppInstUtilInitialize", &init) ||
        !resolveSym(handle, "sceAppInstUtilAppInstallPkg", &inst)) {
        XC_LOGW("update: AppInst simboli assenti");
        return false;
    }
    stampEnter("sceAppInstUtilInitialize");
    int rc = init();
    stampLeave("sceAppInstUtilInitialize", rc);
    stampEnter("sceAppInstUtilAppInstallPkg");
    XC_LOGI("update: AppInstallPkg path %s (niente PrepareOverwrite)", path);
    int ir = inst(path, nullptr);
    stampLeave("sceAppInstUtilAppInstallPkg", ir);
    if (ir != 0) return false;
    using AppProg = int32_t (*)(const char*, uint32_t*);
    using AppExists = int32_t (*)(const char*, int32_t*);
    AppProg gp = nullptr;
    AppExists exists = nullptr;
    resolveSym(handle, "sceAppInstUtilGetInstallProgress", &gp);
    resolveSym(handle, "sceAppInstUtilAppExists", &exists);
    for (int i = 0; i < 60; ++i) {
        uint32_t pct = 0;
        int gr = gp ? gp(kContentId, &pct) : -1;
        int32_t there = 0;
        int er = exists ? exists(kTitleId, &there) : -1;
        if (i % 4 == 0)
            XC_LOGI("update: appinst progress %u rc 0x%08x exists %d er 0x%08x", pct, static_cast<unsigned>(gr), there,
                    static_cast<unsigned>(er));
        if (gr == 0 && pct >= 100) return true;
        if (there == 1 && i >= 4 && (gr != 0 || pct == 0 || pct >= 100)) return true;
        platform::sleepMs(500);
    }
    XC_LOGW("update: appinst progress non a 100, InstallPkg aveva tornato 0");
    return false;
}

// ItemzFlow chiama sceBgftServiceInit (non IntInit) con heap azzerato da 1 MiB.
// IntInit sulla 01.19 ha tornato 0x80990050: sessione assente, non un heap
// troppo piccolo. Il terzo campo è la priorità del thread, usata se il
// firmware la legge oltre heap/size.
struct BgftInitEx {
    void* heap;
    size_t heapSize;
    int32_t priority;
    int32_t pad;
};

// package_size è unsigned long nel sorgente di ItemzFlow, non uint32.
struct BgftParam {
    int32_t userId;
    int32_t entitlementType;
    const char* id;
    const char* contentUrl;
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    int32_t option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    unsigned long packageSize;
};

struct BgftParamEx {
    BgftParam param;
    uint32_t slot;
};

struct BgftProgEx {
    uint32_t bits;
    int32_t errorResult;
    unsigned long length;
    unsigned long transferred;
    unsigned long lengthTotal;
    unsigned long transferredTotal;
    uint32_t numIndex;
    uint32_t numTotal;
    uint32_t restSec;
    uint32_t restSecTotal;
    int32_t preparingPercent;
    int32_t localCopyPercent;
};

// ItemzFlow legge i progress con unsigned long (questo struct). L'header
// OpenOrbis li ha uint32, con preparingPercent a offset 40. Si accetta il
// completamento solo se una delle due letture è un pkg (fino a 32 MiB) e
// transferred ha raggiunto length: una percentuale sull'offset sbagliato
// non deve far credere che l'installazione sia finita.
static_assert(offsetof(BgftParam, packageSize) == 96, "packageSize");
static_assert(offsetof(BgftParamEx, slot) == 104, "slot");
static_assert(offsetof(BgftProgEx, length) == 8, "len");
static_assert(offsetof(BgftProgEx, preparingPercent) == 56, "prep");

bool bgftProgressDone(const BgftProgEx& p) {
    constexpr unsigned long kMax = 32ul * 1024 * 1024;
    auto fits = [](unsigned long len, unsigned long got) {
        return len > 0 && len <= kMax && got >= len && got <= kMax;
    };
    if (fits(p.length, p.transferred) || fits(p.lengthTotal, p.transferredTotal)) return true;
    auto* w = reinterpret_cast<const uint32_t*>(&p);
    uint32_t len = w[2], got = w[3], lenT = w[4], gotT = w[5];
    if (fits(len, got) || fits(lenT, gotT)) return true;
    int32_t prep = static_cast<int32_t>(w[10]);
    int32_t copy = static_cast<int32_t>(w[11]);
    return len > 0 && len <= kMax && prep >= 0 && prep <= 100 && copy >= 0 && copy <= 100 && (prep >= 100 || copy >= 100);
}

using BgftInit = int32_t (*)(void*);
using BgftRegEx = int32_t (*)(BgftParamEx*, int32_t*);
using BgftStart = int32_t (*)(int32_t);
using BgftProgFn = int32_t (*)(int32_t, BgftProgEx*);

bool bgftReady(int rc) {
    uint32_t u = static_cast<uint32_t>(rc);
    return rc == 0 || u == kBgftAlreadyInit || u == kBgftNoSession;
}

int callBgftInit(BgftInit fn, const char* name, void* params) {
    if (!fn) return -1;
    XC_LOGI("update: %s enter", name);
    int rc = fn(params);
    XC_LOGI("update: %s leave 0x%08x", name, static_cast<unsigned>(rc));
    return rc;
}

// BGFT lavora in ShellCore: dopo StartTask il sistema continua anche se
// questa app esce. Per sovrascrivere il titolo già installato si usa
// FORCE_UPDATE; non si chiama mai UnInstall né PrepareOverwritePkg.
// Ritorna true se il task è partito (o era già in corso): il chiamante
// chiude e riapre mentre ShellCore finisce.
bool installWithBgft(const char* path, uint32_t size) {
    tryRaiseCreds();
    const char* paths[] = {"/system/priv/lib/libSceBgft.sprx", "/system/common/lib/libSceBgft.sprx"};
    int32_t handle = loadModule("BGFT", paths, 2, ORBIS_SYSMODULE_INTERNAL_BGFT, "sceBgftServiceIntInit");
    BgftInit initPub = nullptr;
    BgftInit initInt = nullptr;
    BgftRegEx reg = nullptr;
    BgftStart start = nullptr;
    BgftStart startInt = nullptr;
    BgftProgFn prog = nullptr;
    resolveSym(handle, "sceBgftServiceInit", &initPub);
    resolveSym(handle, "sceBgftServiceIntInit", &initInt);
    if (!resolveSym(handle, "sceBgftServiceIntDownloadRegisterTaskByStorageEx", &reg) || (!initPub && !initInt)) {
        XC_LOGW("update: BGFT simboli assenti");
        return false;
    }
    resolveSym(handle, "sceBgftServiceDownloadStartTask", &start);
    resolveSym(handle, "sceBgftServiceIntDownloadStartTask", &startInt);
    resolveSym(handle, "sceBgftServiceDownloadGetProgress", &prog);
    if (!start && !startInt) {
        XC_LOGW("update: BGFT StartTask assente");
        return false;
    }
    // Il servizio tiene l'heap per tutta la vita del processo: non si libera
    // dopo l'init, altrimenti BGFT scrive in memoria già rilasciata.
    static void* heap = nullptr;
    if (!heap) heap = std::calloc(1, 1 << 20);
    if (!heap) {
        XC_LOGW("update: bgft heap assente");
        return false;
    }
    BgftInitEx ip{};
    ip.heap = heap;
    ip.heapSize = 1 << 20;
    ip.priority = 0;
    int rc = -1;
    if (initPub) {
        rc = callBgftInit(initPub, "sceBgftServiceInit", &ip);
        if (static_cast<uint32_t>(rc) == kBgftNoSession) {
            ip.priority = 256;
            rc = callBgftInit(initPub, "sceBgftServiceInit priority", &ip);
        }
    }
    if (initInt && rc != 0 && static_cast<uint32_t>(rc) != kBgftAlreadyInit) {
        ip.priority = 0;
        int rc2 = callBgftInit(initInt, "sceBgftServiceIntInit", &ip);
        if (static_cast<uint32_t>(rc2) == kBgftNoSession) {
            ip.priority = 256;
            rc2 = callBgftInit(initInt, "sceBgftServiceIntInit priority", &ip);
        }
        rc = rc2;
    }
    if (!bgftReady(rc)) return false;
    if (static_cast<uint32_t>(rc) == kBgftNoSession || static_cast<uint32_t>(rc) == kBgftAlreadyInit)
        XC_LOGI("update: bgft già inizializzato, registro il task");
    int32_t user = currentUserId();
    if (user <= 0) return false;
    auto fill = [&](BgftParamEx& ex, int32_t uid, uint32_t bytes, int32_t opt) {
        ex = {};
        ex.param.userId = uid;
        ex.param.entitlementType = 5;
        ex.param.id = "";
        ex.param.contentUrl = path;
        ex.param.contentExUrl = "";
        ex.param.contentName = "PSBox";
        ex.param.iconPath = "";
        ex.param.skuId = "";
        ex.param.option = opt;
        ex.param.playgoScenarioId = "0";
        ex.param.releaseDate = "";
        ex.param.packageType = "";
        ex.param.packageSubType = "";
        ex.param.packageSize = bytes;
        ex.slot = 0;
    };
    // Self-update: FORCE_UPDATE subito. Itemzflow chiede conferma e fa
    // UnInstall: qui non si disinstalla mai la versione corrente.
    const int32_t kForce = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM | ORBIS_BGFT_TASK_OPT_FORCE_UPDATE;
    int32_t task = -1;
    BgftParamEx ex{};
    fill(ex, user, size, kForce);
    stampEnter("DownloadRegisterTaskByStorageEx");
    XC_LOGI("update: BGFT register size %u user %d opt 0x%x FORCE_UPDATE", size, user, kForce);
    int rr = reg(&ex, &task);
    stampLeave("DownloadRegisterTaskByStorageEx", rr);
    XC_LOGI("update: BGFT task %d", task);
    uint32_t urr = static_cast<uint32_t>(rr);
    if (urr == kBgftBusy) {
        XC_LOGI("update: BGFT task già in corso, lascio lavorare ShellCore");
        return true;
    }
    if (urr == kBgftInvalidArg) {
        XC_LOGW("update: BGFT argomento rifiutato (user %d)", user);
        return false;
    }
    if ((urr == kBgftAlreadyInstalled || urr == kBgftExists) && task < 0) {
        // Riprova con le stesse opzioni (già FORCE): a volte il primo giro
        // rifiuta e il secondo accetta dopo che ShellCore ha visto il pkg.
        task = -1;
        stampEnter("DownloadRegisterTaskByStorageEx retry");
        rr = reg(&ex, &task);
        stampLeave("DownloadRegisterTaskByStorageEx retry", rr);
        XC_LOGI("update: BGFT task %d", task);
        urr = static_cast<uint32_t>(rr);
        if (urr == kBgftBusy) return true;
    }
    if (rr != 0 || task < 0) return false;
    int st = -1;
    if (start) {
        stampEnter("sceBgftServiceDownloadStartTask");
        st = start(task);
        stampLeave("sceBgftServiceDownloadStartTask", st);
    }
    if (st != 0 && startInt) {
        stampEnter("sceBgftServiceIntDownloadStartTask");
        st = startInt(task);
        stampLeave("sceBgftServiceIntDownloadStartTask", st);
    }
    if (st != 0) {
        XC_LOGW("update: BGFT StartTask 0x%08x", static_cast<unsigned>(st));
        return false;
    }
    // Breve controllo che il task si muova; poi si esce: restare in attesa
    // mentre ShellCore sovrascrive questa app rischia di uccidere il processo
    // a metà, e il progresso continua comunque fuori da qui.
    if (prog) {
        for (int i = 0; i < 8; ++i) {
            BgftProgEx p{};
            int pr = prog(task, &p);
            XC_LOGI("update: bgft %lu/%lu prep %d err 0x%08x (rc 0x%08x)", p.transferred, p.length, p.preparingPercent,
                    static_cast<unsigned>(p.errorResult), static_cast<unsigned>(pr));
            if (p.errorResult != 0) {
                XC_LOGW("update: bgft errore subito 0x%08x", static_cast<unsigned>(p.errorResult));
                return false;
            }
            if (bgftProgressDone(p)) {
                XC_LOGI("update: bgft già completo in processo");
                return true;
            }
            platform::sleepMs(500);
        }
    }
    XC_LOGI("update: bgft avviato, ShellCore continua dopo l'uscita");
    return true;
}

using LoadExecFn = int32_t (*)(const char*, const char**);
using LncInit = int32_t (*)();
using LncLaunch = int32_t (*)(const char*, const char**, LncAppParam*);

bool relaunch() {
    XC_LOGI("update: relaunch enter");
    const char* sysPaths[] = {"/system/priv/lib/libSceSystemService.sprx", "/system/common/lib/libSceSystemService.sprx"};
    int32_t sys = loadModule("SystemService", sysPaths, 2, 0x80000010, "sceSystemServiceLoadExec");
    LoadExecFn loadExec = nullptr;
    const char* argv[] = {nullptr};
    if (resolveSym(sys, "sceSystemServiceLoadExec", &loadExec)) {
        XC_LOGI("update: sceSystemServiceLoadExec enter");
        int rc = loadExec("/app0/eboot.bin", argv);
        XC_LOGI("update: sceSystemServiceLoadExec leave 0x%08x", static_cast<unsigned>(rc));
    } else {
        XC_LOGW("update: sceSystemServiceLoadExec assente");
    }
    const char* lncPaths[] = {"/system/priv/lib/libSceLncUtil.sprx", "/system/common/lib/libSceLncUtil.sprx"};
    int32_t lnc = loadModule("Lnc", lncPaths, 2, 0, "sceLncUtilLaunchApp");
    LncInit init = nullptr;
    LncLaunch launch = nullptr;
    if (!resolveSym(lnc, "sceLncUtilInitialize", &init) || !resolveSym(lnc, "sceLncUtilLaunchApp", &launch)) {
        XC_LOGW("update: Lnc simboli assenti");
        return false;
    }
    XC_LOGI("update: sceLncUtilInitialize enter");
    int ir = init();
    XC_LOGI("update: sceLncUtilInitialize leave 0x%08x", static_cast<unsigned>(ir));
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);
    LncAppParam param{};
    param.size = sizeof param;
    param.user_id = static_cast<uint32_t>(user);
    param.LaunchAppCheck_flag = LaunchApp_SkipSystemUpdate;
    const char* largv[] = {nullptr};
    XC_LOGI("update: sceLncUtilLaunchApp enter");
    int lrc = launch(kTitleId, largv, &param);
    XC_LOGI("update: sceLncUtilLaunchApp leave 0x%08x", static_cast<unsigned>(lrc));
    return lrc == 0;
}

}  // namespace

LocalUpdateResult applyLocalUpdate(const std::string& current) {
    LocalUpdateResult r;
    XC_LOGI("update: controllo %s (questa è %s)", kUpdateDir, current.c_str());
    {
        // Un aggiornamento precedente ha lasciato il marker: se siamo già
        // alla versione richiesta, il BGFT ha finito. Il pkg in /data/pkg resta
        // finché non è più nuova della build corrente.
        std::string pending = readPending();
        if (!pending.empty()) {
            PkgVer pend = parsePkgVer(pending);
            PkgVer cur = parsePkgVer(current);
            XC_LOGI("update: pending '%s' corrente %s", pending.c_str(), current.c_str());
            if (pend.ok && cur.ok && cmpPkgVer(cur, pend) >= 0) {
                clearPending();
                XC_LOGI("update: installazione già applicata");
            }
        }
    }
    auto names = listPkgs(kUpdateDir);
    std::string srcDir = kUpdateDir;
    if (names.empty()) {
        // Dopo un tentativo interrotto il pkg può essere già in /data/pkg
        // (la 01.23 c'è rimasta mentre l'icona spariva).
        names = listPkgs(kPkgDir);
        srcDir = kPkgDir;
        if (!names.empty()) XC_LOGI("update: uso i pkg già in %s", kPkgDir);
    }
    if (names.empty()) {
        XC_LOGI("update: nessun PSBox-PS4-*.pkg");
        return r;
    }
    PkgVer cur = parsePkgVer(current);
    std::string bestName;
    PkgVer best{};
    std::string bestLabel;
    long long bestBytes = 0;
    bool hardReject = false;
    for (const auto& name : names) {
        std::string path = srcDir + "/" + name;
        Measured m = measurePkg(path);
        XC_LOGI("update: misura %s lseek %lld stat48 %lld magic %02x%02x%02x%02x", name.c_str(), m.lseekBytes, m.stat48,
                m.mag[0], m.mag[1], m.mag[2], m.mag[3]);
        std::string bytes;
        std::string fromSfo;
        if (m.magic && m.lseekBytes >= 1024 && m.lseekBytes <= 32ll * 1024 * 1024 && readAll(path, bytes))
            fromSfo = appVerFromPkg(bytes);
        PkgVer vs = parsePkgVer(fromSfo);
        PkgVer vf = parsePkgVer(name);
        PkgVer use = vs.ok ? vs : vf;
        XC_LOGI("update: %s sfo '%s' (%s) nome %s", name.c_str(), fromSfo.c_str(), vs.ok ? pkgVerText(vs).c_str() : "-",
                vf.ok ? pkgVerText(vf).c_str() : "-");
        if (m.lseekBytes < 1024) {
            hardReject = true;
            XC_LOGI("update: scarto %s motivo dimensione %lld", name.c_str(), m.lseekBytes);
            continue;
        }
        if (!m.magic) {
            hardReject = true;
            XC_LOGI("update: scarto %s motivo magic", name.c_str());
            continue;
        }
        if (!use.ok) {
            hardReject = true;
            XC_LOGI("update: scarto %s motivo versione illeggibile (sfo '%s' nome '%s')", name.c_str(), fromSfo.c_str(),
                    name.c_str());
            continue;
        }
        if (cur.ok && cmpPkgVer(use, cur) <= 0) {
            XC_LOGI("update: scarto %s motivo %s non è più nuova di %s", name.c_str(), pkgVerText(use).c_str(),
                    pkgVerText(cur).c_str());
            continue;
        }
        if (bestName.empty() || cmpPkgVer(use, best) > 0) {
            best = use;
            bestName = name;
            bestLabel = vs.ok ? fromSfo : pkgVerText(vf);
            bestBytes = m.lseekBytes;
        }
    }
    if (bestName.empty()) {
        if (hardReject) {
            r.status = LocalUpdateStatus::Failed;
            r.detail = "pkg scartato";
        }
        XC_LOGI("update: nessun pkg più nuovo");
        return r;
    }
    r.version = bestLabel;
    XC_LOGI("update: scelto %s versione %s dimensione %lld magic 7f434e54", bestName.c_str(), pkgVerText(best).c_str(),
            bestBytes);
    if (mkdir(kPkgDir, 0755) != 0 && errno != EEXIST) XC_LOGW("update: mkdir %s (%d)", kPkgDir, errno);
    std::string src = srcDir + "/" + bestName;
    std::string dst = std::string(kPkgDir) + "/" + bestName;
    if (src != dst) {
        if (rename(src.c_str(), dst.c_str()) != 0) {
            XC_LOGI("update: rename non riuscita (%d), copio", errno);
            if (!copyFile(src, dst)) {
                r.status = LocalUpdateStatus::Failed;
                r.detail = "copia fallita";
                XC_LOGW("update: copia in %s fallita", dst.c_str());
                return r;
            }
            if (unlink(src.c_str()) != 0) XC_LOGW("update: originale non rimosso (%d)", errno);
            XC_LOGI("update: copiato in %s", dst.c_str());
        } else {
            XC_LOGI("update: spostato in %s", dst.c_str());
        }
    } else {
        XC_LOGI("update: pkg già in %s", dst.c_str());
    }
    unlinkOthers(kPkgDir, bestName);
    Measured placed = measurePkg(dst);
    XC_LOGI("update: pkg %s, lseek %lld stat48 %lld", dst.c_str(), placed.lseekBytes, placed.stat48);
    if (placed.lseekBytes < 1024 || !placed.magic) {
        r.status = LocalUpdateStatus::Failed;
        r.detail = "pkg a destinazione non valido";
        XC_LOGI("update: scarto %s motivo dopo lo spostamento lseek %lld magic %d", bestName.c_str(), placed.lseekBytes,
                placed.magic ? 1 : 0);
        return r;
    }
    // Prima BGFT (ShellCore, sopravvive alla chiusura), poi AppInstallPkg
    // senza PrepareOverwrite. Mai disinstallare la build corrente.
    uint32_t pkgSize = placed.lseekBytes > 0xffffffffll ? 0xffffffffu : static_cast<uint32_t>(placed.lseekBytes);
    writePending(pkgVerText(best));
    stampEnter("install BGFT");
    bool ok = installWithBgft(dst.c_str(), pkgSize);
    stampLeave("install BGFT", ok ? 0 : -1);
    if (!ok) {
        XC_LOGW("update: BGFT non è bastato, provo AppInstallPkg senza overwrite");
        stampEnter("install AppInst");
        ok = installWithAppInst(dst.c_str());
        stampLeave("install AppInst", ok ? 0 : -1);
    }
    if (!ok) {
        clearPending();
        r.status = LocalUpdateStatus::Failed;
        r.detail = kManualInstall;
        XC_LOGW("update: %s (%s)", kManualInstall, dst.c_str());
        platform::notify(kManualInstall, std::string("update: ") + kManualInstall);
        return r;
    }
    // Non svuotare /data/pkg: se l'app muore a metà, il pkg resta per un
    // nuovo tentativo o per l'installazione a mano. Solo la cartella update.
    clearUpdateDir();
    XC_LOGI("update: task avviato per %s, chiudo e riapre", pkgVerText(best).c_str());
    platform::notify("Installazione in corso", "update: ShellCore installa, riapertura");
    platform::sleepMs(800);
    stampEnter("relaunch");
    relaunch();
    stampLeave("relaunch", -1);
    r.status = LocalUpdateStatus::Installed;
    r.detail = "riavvio non partito";
    XC_LOGW("update: riavvio non partito, _exit per lasciare lavorare ShellCore");
    xc_log_stamp("update: _exit dopo install");
    _exit(0);
    return r;
}

}  // namespace xc::app
