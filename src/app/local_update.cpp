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

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "psbox_install_bin.h"

namespace xc::app {
namespace {

// File I/O solo su /data/... (vista sandbox = /user/data). BGFT (ShellCore)
// vuole la stringa /user/data/... sullo stesso inode (FPKGi #31). Mai mkdir/
// open/copy sotto /user — in sandbox errno 2 (01.41).
constexpr const char* kUpdateDir = "/data/psbox/update";
constexpr const char* kPkgDirLegacy = "/data/pkg";
[[maybe_unused]] constexpr const char* kTitleId = "XCLD00001";
[[maybe_unused]] constexpr const char* kContentId = "IV0000-XCLD00001_00-PSBOXCLOUD000001";
constexpr const char* kPendingPath = "/data/psbox/update.pending";
constexpr const char* kManualInstall = "Installa a mano con Package Installer";

// Solo per contentUrl BGFT (codice disattivato). Non usarlo per I/O.
[[maybe_unused]] std::string toBgftUrl(const std::string& dataPath) {
    if (dataPath.rfind("/user/", 0) == 0) return dataPath;
    if (dataPath.rfind("/data/", 0) == 0) return "/user" + dataPath;
    return dataPath;
}

bool ensureDir(const char* path) {
    // Mai /user/...: in sandbox non esiste.
    if (path && std::strncmp(path, "/user/", 6) == 0) {
        XC_LOGW("[UPDATE] mkdir %s saltato (solo /data per I/O)", path);
        return false;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
    XC_LOGW("[UPDATE] mkdir %s errno %d", path, errno);
    return false;
}
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
    std::snprintf(line, sizeof line, "[UPDATE] %s enter", what ? what : "?");
    xc_log_stamp(line);
    XC_LOGI("%s", line);
}

void stampLeave(const char* what, int rc) {
    char line[160];
    std::snprintf(line, sizeof line, "[UPDATE] %s leave 0x%08x", what ? what : "?", static_cast<unsigned>(rc));
    xc_log_stamp(line);
    XC_LOGI("%s", line);
}

void updLog(const char* fmt, ...) {
    char body[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    char line[280];
    std::snprintf(line, sizeof line, "[UPDATE] %s", body);
    xc_log_stamp(line);
    XC_LOGI("%s", line);
}

void updWarn(const char* fmt, ...) {
    char body[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    char line[280];
    std::snprintf(line, sizeof line, "[UPDATE] %s", body);
    xc_log_stamp(line);
    XC_LOGW("%s", line);
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
            // UTF-8: 0x0004 o 0x0204 (PkgTool). 0x0404 = int32.
            // Bug 01.38: (fmt & 0x0400)==0 scartava 0x0204 → sfo sempre ''.
            if (fmt != 0x0004 && fmt != 0x0204) continue;
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

// FTP ancora in corso: la size deve restare stabile per ~300 ms.
bool waitStableSize(const std::string& path, Measured& out) {
    long long prev = -1;
    int stable = 0;
    for (int i = 0; i < 40; ++i) {
        Measured m = measurePkg(path);
        if (!m.exists) {
            updWarn("pkg sparito durante l'attesa: %s", path.c_str());
            return false;
        }
        if (m.lseekBytes > 0 && m.lseekBytes == prev) {
            if (++stable >= 3) {
                out = m;
                updLog("size stabile %lld dopo %d check", m.lseekBytes, i + 1);
                return true;
            }
        } else {
            stable = 0;
            if (i == 0 || m.lseekBytes != prev)
                updLog("size in movimento %lld → %lld (FTP?)", prev, m.lseekBytes);
            prev = m.lseekBytes;
        }
        platform::sleepMs(100);
    }
    out = measurePkg(path);
    updWarn("size non stabilizzata (ultimo %lld) — uso comunque", out.lseekBytes);
    return out.exists && out.lseekBytes >= 1024;
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
[[maybe_unused]] bool tryRaiseCreds() {
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

// 01.43: sceAppInstUtilAppInstallPkg ha tornato 0x80020012 e ha DANNEGGIATO
// l'app installata. BGFT ha tornato 0x80990088. Non si chiamano più da qui.
#if 0
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

// Attende fine/errore del task BGFT. Non uscire prima: 01.39 usciva subito
// e ShellCore lasciava il titolo «danneggiata».
bool waitBgftTask(BgftProgFn prog, int32_t task) {
    if (!prog || task < 0) {
        updWarn("BGFT progress assente — non posso verificare la fine");
        return false;
    }
    constexpr int kMaxTicks = 600;  // 600 × 500 ms ≈ 5 min
    for (int i = 0; i < kMaxTicks; ++i) {
        BgftProgEx p{};
        stampEnter("DownloadGetProgress");
        int pr = prog(task, &p);
        stampLeave("DownloadGetProgress", pr);
        updLog("bgft task %d %lu/%lu tot %lu/%lu prep %d copy %d err 0x%08x rc 0x%08x", task, p.transferred,
               p.length, p.transferredTotal, p.lengthTotal, p.preparingPercent, p.localCopyPercent,
               static_cast<unsigned>(p.errorResult), static_cast<unsigned>(pr));
        if (p.errorResult != 0) {
            updWarn("bgft errore task 0x%08x", static_cast<unsigned>(p.errorResult));
            return false;
        }
        if (bgftProgressDone(p)) {
            updLog("bgft task %d completo", task);
            return true;
        }
        platform::sleepMs(500);
    }
    updWarn("bgft timeout dopo ~5 min (task %d)", task);
    return false;
}

// Ordine Itemzflow/flatz: AppInstUtilInitialize PRIMA di BGFT init/register.
// `dataPath` = path sandbox (/data/...). contentUrl prova /user+dataPath, poi
// fallback /data/... (log di entrambi i rc). Mai UnInstall.
bool installWithBgft(const char* dataPath, uint32_t size) {
    std::string ioPath = dataPath ? dataPath : "";
    if (ioPath.rfind("/data/", 0) != 0) {
        updWarn("BGFT I/O path non-/data: %s", ioPath.c_str());
        return false;
    }
    std::string urlUser = toBgftUrl(ioPath);
    static char urlBuf[512];
    std::snprintf(urlBuf, sizeof urlBuf, "%s", urlUser.c_str());
    updLog("BGFT contentUrl %s (file I/O %s) size %u", urlBuf, ioPath.c_str(), size);

    tryRaiseCreds();
    {
        const char* ap[] = {"/system/common/lib/libSceAppInstUtil.sprx", "/system/priv/lib/libSceAppInstUtil.sprx"};
        int32_t ah = loadModule("AppInst", ap, 2, ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL, "sceAppInstUtilInitialize");
        AppInit init = nullptr;
        if (resolveSym(ah, "sceAppInstUtilInitialize", &init) && init) {
            stampEnter("sceAppInstUtilInitialize(pre-BGFT)");
            int irc = init();
            stampLeave("sceAppInstUtilInitialize(pre-BGFT)", irc);
        } else {
            updWarn("AppInst init pre-BGFT non disponibile — provo comunque BGFT");
        }
    }
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
        updWarn("BGFT simboli assenti");
        return false;
    }
    resolveSym(handle, "sceBgftServiceDownloadStartTask", &start);
    resolveSym(handle, "sceBgftServiceIntDownloadStartTask", &startInt);
    resolveSym(handle, "sceBgftServiceDownloadGetProgress", &prog);
    if (!start && !startInt) {
        updWarn("BGFT StartTask assente");
        return false;
    }
    static void* heap = nullptr;
    if (!heap) heap = std::calloc(1, 1 << 20);
    if (!heap) {
        updWarn("bgft heap assente");
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
    if (!bgftReady(rc)) {
        updWarn("BGFT init non pronto rc 0x%08x", static_cast<unsigned>(rc));
        return false;
    }
    if (static_cast<uint32_t>(rc) == kBgftNoSession || static_cast<uint32_t>(rc) == kBgftAlreadyInit)
        updLog("bgft già inizializzato, registro il task");
    int32_t user = currentUserId();
    if (user <= 0) return false;
    // Niente DELETE_AFTER_UPLOAD: cancelliamo noi su /data solo dopo wait OK.
    const int32_t kOpt = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM | ORBIS_BGFT_TASK_OPT_FORCE_UPDATE;

    auto fill = [&](BgftParamEx& ex, const char* url) {
        ex = {};
        ex.param.userId = user;
        ex.param.entitlementType = 5;
        ex.param.id = "";
        ex.param.contentUrl = url;
        ex.param.contentExUrl = "";
        ex.param.contentName = "PSBox";
        ex.param.iconPath = "";
        ex.param.skuId = "";
        ex.param.option = kOpt;
        ex.param.playgoScenarioId = "0";
        ex.param.releaseDate = "";
        ex.param.packageType = "";
        ex.param.packageSubType = "";
        ex.param.packageSize = size;
        ex.slot = 0;
    };

    auto registerOnce = [&](const char* url, int32_t* taskOut) -> int {
        BgftParamEx ex{};
        fill(ex, url);
        stampEnter("DownloadRegisterTaskByStorageEx");
        updLog("BGFT register size %u user %d opt 0x%x path %s", size, user, kOpt, url);
        int rr = reg(&ex, taskOut);
        stampLeave("DownloadRegisterTaskByStorageEx", rr);
        updLog("BGFT task %d rc 0x%08x path %s", *taskOut, static_cast<unsigned>(rr), url);
        return rr;
    };

    int32_t task = -1;
    int rr = registerOnce(urlBuf, &task);
    uint32_t urr = static_cast<uint32_t>(rr);
    // Se /user/data viene rifiutato, riprova con /data/... e logga entrambi.
    if (rr != 0 && urr != kBgftBusy && urr != kBgftAlreadyInstalled && urr != kBgftExists) {
        static char urlData[512];
        std::snprintf(urlData, sizeof urlData, "%s", ioPath.c_str());
        int32_t task2 = -1;
        int rr2 = registerOnce(urlData, &task2);
        updWarn("BGFT register /user rc 0x%08x, fallback /data rc 0x%08x", urr, static_cast<unsigned>(rr2));
        rr = rr2;
        urr = static_cast<uint32_t>(rr2);
        task = task2;
        std::snprintf(urlBuf, sizeof urlBuf, "%s", urlData);
    }

    if (urr == kBgftAlreadyInstalled || urr == kBgftExists) {
        updWarn("BGFT 0x%08x già installato/patch — niente UnInstall, usa Package Installer", urr);
        return false;
    }
    if (urr == kBgftBusy) {
        updLog("BGFT task già in corso — attendo progresso");
        if (task < 0) return false;
    } else if (rr != 0 || task < 0) {
        updWarn("BGFT register fallito rc 0x%08x task %d", urr, task);
        return false;
    }
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
    if (st != 0 && urr != kBgftBusy) {
        updWarn("BGFT StartTask 0x%08x", static_cast<unsigned>(st));
        return false;
    }
    stampEnter("waitBgftTask");
    bool done = waitBgftTask(prog, task);
    stampLeave("waitBgftTask", done ? 0 : -1);
    return done;
}
#endif  // AppInst/BGFT disattivati in 01.45

using LoadExecFn = int32_t (*)(const char*, const char**);

// Dopo install completo: LoadExec("exit"). Niente relaunch del titolo in corso
// (non provato in open-source). L'utente riapre da home.
bool exitAfterUpdate() {
    updLog("LoadExec exit dopo install completo");
    const char* sysPaths[] = {"/system/priv/lib/libSceSystemService.sprx", "/system/common/lib/libSceSystemService.sprx"};
    int32_t sys = loadModule("SystemService", sysPaths, 2, 0x80000010, "sceSystemServiceLoadExec");
    LoadExecFn loadExec = nullptr;
    if (!resolveSym(sys, "sceSystemServiceLoadExec", &loadExec) || !loadExec) {
        updWarn("sceSystemServiceLoadExec assente");
        return false;
    }
    stampEnter("sceSystemServiceLoadExec(exit)");
    int rc = loadExec("exit", nullptr);
    stampLeave("sceSystemServiceLoadExec(exit)", rc);
    return false;  // se torna, fallito
}

// Sceglie il pkg più nuovo tra le cartelle di scan (nessuna modifica file).
bool pickNewerPkg(const std::string& current, std::string& outPath, std::string& outVer, std::string& outName,
                  long long& outBytes, bool& hardReject) {
    hardReject = false;
    outPath.clear();
    outVer.clear();
    outName.clear();
    outBytes = 0;
    ensureDir("/data/psbox");
    ensureDir(kUpdateDir);
    const char* dirs[] = {kUpdateDir, kPkgDirLegacy};
    PkgVer cur = parsePkgVer(current);
    PkgVer best{};
    for (const char* dir : dirs) {
        for (const auto& name : listPkgs(dir)) {
            std::string path = std::string(dir) + "/" + name;
            Measured m{};
            if (!waitStableSize(path, m)) {
                hardReject = true;
                updWarn("scarto %s: upload incompleto / assente", name.c_str());
                continue;
            }
            updLog("misura %s lseek %lld magic %02x%02x%02x%02x", name.c_str(), m.lseekBytes, m.mag[0], m.mag[1],
                   m.mag[2], m.mag[3]);
            if (m.lseekBytes < 1024 || m.lseekBytes > 32ll * 1024 * 1024 || !m.magic) {
                hardReject = true;
                updWarn("scarto %s: size/magic non validi", name.c_str());
                continue;
            }
            std::string bytes, fromSfo;
            if (readAll(path, bytes)) fromSfo = appVerFromPkg(bytes);
            PkgVer vs = parsePkgVer(fromSfo);
            PkgVer vf = parsePkgVer(name);
            PkgVer use = vs.ok ? vs : vf;
            updLog("%s sfo '%s' (%s) nome %s", name.c_str(), fromSfo.c_str(), vs.ok ? pkgVerText(vs).c_str() : "-",
                   vf.ok ? pkgVerText(vf).c_str() : "-");
            if (!use.ok) {
                hardReject = true;
                updWarn("scarto %s: versione illeggibile", name.c_str());
                continue;
            }
            if (cur.ok && cmpPkgVer(use, cur) <= 0) {
                updLog("scarto %s: %s non più nuova di %s", name.c_str(), pkgVerText(use).c_str(),
                       pkgVerText(cur).c_str());
                continue;
            }
            if (outPath.empty() || cmpPkgVer(use, best) > 0) {
                best = use;
                outName = name;
                outVer = vs.ok ? fromSfo : pkgVerText(vf);
                outPath = path;
                outBytes = m.lseekBytes;
            }
        }
    }
    return !outPath.empty();
}

}  // namespace

LocalUpdateResult findLocalUpdate(const std::string& current) {
    LocalUpdateResult r;
    updLog("scan (no install) %s (questa è %s)", kUpdateDir, current.c_str());
    {
        std::string pending = readPending();
        if (!pending.empty()) {
            PkgVer pend = parsePkgVer(pending);
            PkgVer cur = parsePkgVer(current);
            updLog("pending '%s' corrente %s", pending.c_str(), current.c_str());
            if (pend.ok && cur.ok && cmpPkgVer(cur, pend) >= 0) {
                clearPending();
                updLog("installazione già applicata");
            }
        }
    }
    std::string path, ver, name;
    long long bytes = 0;
    bool hardReject = false;
    if (!pickNewerPkg(current, path, ver, name, bytes, hardReject)) {
        if (hardReject) {
            r.status = LocalUpdateStatus::Failed;
            r.detail = "pkg scartato";
        }
        updLog("nessun pkg più nuovo → Nessun aggiornamento");
        return r;
    }
    r.status = LocalUpdateStatus::Available;
    r.version = ver;
    r.path = path;
    r.detail = name;
    updLog("disponibile %s versione %s size %lld path %s — attendo scelta utente", name.c_str(), ver.c_str(), bytes,
           path.c_str());
    return r;
}

// --- GoldHEN: RPI :12800 oppure BinLoader :9090. Mai AppInst/BGFT. --------

bool setNonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

// connect con timeout. fd chiuso se fallisce.
bool tcpOpen(uint32_t addrHost, int port, int timeoutMs, int& fdOut) {
    fdOut = -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        updWarn("socket errno %d", errno);
        return false;
    }
    setNonblock(fd);
    sockaddr_in addr{};
    addr.sin_len = sizeof addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(addrHost);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    if (rc != 0 && errno != EINPROGRESS && errno != EALREADY) {
        updWarn("connect 127.0.0.1:%d errno %d", port, errno);
        close(fd);
        return false;
    }
    if (rc != 0) {
        pollfd p{fd, POLLOUT, 0};
        int pr = poll(&p, 1, timeoutMs);
        if (pr <= 0) {
            updWarn("connect 127.0.0.1:%d timeout", port);
            close(fd);
            return false;
        }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) {
            updWarn("connect 127.0.0.1:%d so_error %d", port, err);
            close(fd);
            return false;
        }
    }
    fdOut = fd;
    return true;
}

bool sendAll(int fd, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, data + off, n - off, 0);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd p{fd, POLLOUT, 0};
                if (poll(&p, 1, 2000) <= 0) return false;
                continue;
            }
            return false;
        }
        if (w == 0) return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

bool readSome(int fd, std::string& out, int timeoutMs, size_t cap) {
    char buf[2048];
    int waited = 0;
    while (out.size() < cap && waited < timeoutMs) {
        pollfd p{fd, POLLIN, 0};
        int pr = poll(&p, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0) {
            waited += 200;
            if (!out.empty()) break;
            continue;
        }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return !out.empty();
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return !out.empty();
}

bool httpExchange(int port, const std::string& req, std::string& resp) {
    int fd = -1;
    if (!tcpOpen(0x7f000001u, port, 800, fd)) return false;
    bool ok = sendAll(fd, req.data(), req.size()) && readSome(fd, resp, 4000, 8192);
    close(fd);
    return ok;
}

// Server HTTP locale: un GET qualunque riceve il pkg. Gira finché stop.
void servePkg(int listenFd, const std::string& path, std::atomic<bool>& stop) {
    while (!stop.load()) {
        pollfd p{listenFd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        int c = accept(listenFd, nullptr, nullptr);
        if (c < 0) continue;
        std::string req;
        readSome(c, req, 1500, 2048);
        std::string line = req.substr(0, req.find('\n'));
        if (line.size() > 120) line.resize(120);
        updLog("HTTP richiesta %s", line.c_str());
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            const char* err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            sendAll(c, err, std::strlen(err));
            close(c);
            continue;
        }
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        char hdr[160];
        int hn = std::snprintf(hdr, sizeof hdr,
                               "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                               "Content-Length: %ld\r\nConnection: close\r\n\r\n",
                               sz);
        bool ok = hn > 0 && sendAll(c, hdr, static_cast<size_t>(hn));
        char buf[1 << 16];
        long sent = 0;
        while (ok && sent < sz) {
            size_t n = std::fread(buf, 1, sizeof buf, f);
            if (!n) {
                ok = false;
                break;
            }
            ok = sendAll(c, buf, n);
            sent += static_cast<long>(n);
        }
        std::fclose(f);
        close(c);
        updLog("HTTP serviti %ld/%ld byte ok %d", sent, sz, ok ? 1 : 0);
    }
}

int listenLocal(int& portOut) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_len = sizeof addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(0x7f000001u);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 2) != 0) {
        updWarn("HTTP bind/listen errno %d", errno);
        close(fd);
        return -1;
    }
    socklen_t al = sizeof addr;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &al) != 0) {
        close(fd);
        return -1;
    }
    portOut = ntohs(addr.sin_port);
    setNonblock(fd);
    return fd;
}

int jsonIntAfter(const std::string& body, const char* key) {
    auto at = body.find(key);
    if (at == std::string::npos) return -1;
    at = body.find_first_of("-0123456789", at + std::strlen(key));
    if (at == std::string::npos) return -1;
    return std::atoi(body.c_str() + at);
}

bool jsonStatusFail(const std::string& body) {
    if (body.find("\"fail\"") != std::string::npos) return true;
    bool hasError = body.find("\"error\"") != std::string::npos;
    bool errorNull = body.find("\"error\":null") != std::string::npos || body.find("\"error\": null") != std::string::npos;
    return hasError && !errorNull;
}

// true = GoldHEN ha scaricato il pkg (o sta installando). Poi si chiude l'app.
bool installViaGoldhenRpi(const std::string& pkgPath, const std::string& fileName) {
    updLog("RPI probe 127.0.0.1:12800");
    int probe = -1;
    if (!tcpOpen(0x7f000001u, 12800, 800, probe)) {
        updWarn("RPI non raggiungibile");
        return false;
    }
    close(probe);
    updLog("RPI in ascolto");

    int port = 0;
    int lfd = listenLocal(port);
    if (lfd < 0) {
        updWarn("HTTP locale non partito");
        return false;
    }
    updLog("HTTP locale 127.0.0.1:%d serve %s", port, fileName.c_str());
    std::atomic<bool> stop{false};
    std::thread server(servePkg, lfd, pkgPath, std::ref(stop));

    char url[160];
    std::snprintf(url, sizeof url, "http://127.0.0.1:%d/%s", port, fileName.c_str());
    char json[256];
    int jn = std::snprintf(json, sizeof json, "{\"type\":\"direct\",\"packages\":[\"%s\"]}", url);
    char req[512];
    int rn = std::snprintf(req, sizeof req,
                           "POST /api/install HTTP/1.1\r\nHost: 127.0.0.1:12800\r\n"
                           "Content-Type: application/json\r\nContent-Length: %d\r\n"
                           "Connection: close\r\n\r\n%s",
                           jn, json);
    updLog("RPI POST /api/install %s", url);
    std::string resp;
    bool posted = rn > 0 && httpExchange(12800, std::string(req, static_cast<size_t>(rn)), resp);
    updLog("RPI risposta %s", resp.substr(0, 180).c_str());
    int task = jsonIntAfter(resp, "task_id");
    if (!posted || task < 0 || jsonStatusFail(resp)) {
        updWarn("RPI install rifiutato task %d", task);
        stop = true;
        shutdown(lfd, SHUT_RDWR);
        close(lfd);
        if (server.joinable()) server.join();
        return false;
    }
    updLog("RPI task %d, attendo il download", task);
    bool done = false;
    for (int i = 0; i < 180 && !done; ++i) {
        platform::sleepMs(1000);
        char preq[256];
        int pn = std::snprintf(preq, sizeof preq,
                               "GET /api/get_task_progress?task_id=%d HTTP/1.1\r\nHost: 127.0.0.1:12800\r\n"
                               "Connection: close\r\n\r\n",
                               task);
        std::string prog;
        if (pn <= 0 || !httpExchange(12800, std::string(preq, static_cast<size_t>(pn)), prog)) {
            updWarn("RPI progress assente (giro %d)", i);
            continue;
        }
        int transferred = jsonIntAfter(prog, "transferred");
        int length = jsonIntAfter(prog, "length");
        int prep = jsonIntAfter(prog, "preparing_percent");
        int copy = jsonIntAfter(prog, "local_copy_percent");
        if (i % 2 == 0)
            updLog("RPI progress transferred %d length %d prep %d copy %d", transferred, length, prep, copy);
        if (jsonStatusFail(prog) && transferred <= 0 && i > 2) {
            updWarn("RPI progress in errore");
            break;
        }
        bool downloaded = length > 0 && transferred >= length;
        bool copied = copy >= 100 && transferred > 0 && (length <= 0 || transferred >= length);
        if (downloaded || copied) {
            updLog("RPI download completo (transferred %d/%d prep %d copy %d)", transferred, length, prep, copy);
            done = true;
        }
    }
    stop = true;
    shutdown(lfd, SHUT_RDWR);
    close(lfd);
    if (server.joinable()) server.join();
    return done;
}

bool sendBinloaderPayload() {
    updLog("BinLoader probe 127.0.0.1:9090 (%u byte)", kPsboxInstallBinLen);
    int fd = -1;
    if (!tcpOpen(0x7f000001u, 9090, 800, fd)) {
        updWarn("BinLoader non raggiungibile");
        return false;
    }
    bool ok = sendAll(fd, reinterpret_cast<const char*>(kPsboxInstallBin), kPsboxInstallBinLen);
    shutdown(fd, SHUT_WR);
    close(fd);
    if (!ok) updWarn("BinLoader invio incompleto");
    else updLog("payload consegnato a BinLoader, attende l'uscita dell'app e installa da /data/pkg");
    return ok;
}

bool fsyncPath(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    int rc = fsync(fd);
    close(fd);
    return rc == 0;
}

LocalUpdateResult installLocalUpdate(const std::string& current) {
    LocalUpdateResult r;
    updLog("install richiesto (Sì) corrente %s", current.c_str());
    std::string src, ver, name;
    long long bytes = 0;
    bool hardReject = false;
    if (!pickNewerPkg(current, src, ver, name, bytes, hardReject)) {
        r.status = LocalUpdateStatus::Failed;
        r.detail = hardReject ? "pkg scartato" : "nessun pkg";
        updWarn("install: nessun pkg più nuovo");
        return r;
    }
    r.version = ver;
    r.path = src;
    updLog("install %s versione %s da %s", name.c_str(), ver.c_str(), src.c_str());

    Measured placed{};
    if (!waitStableSize(src, placed) || placed.lseekBytes < 1024 || !placed.magic) {
        r.status = LocalUpdateStatus::Failed;
        r.detail = "pkg non valido";
        updWarn("pkg invalido %s magic %d size %lld", src.c_str(), placed.magic ? 1 : 0, placed.lseekBytes);
        return r;
    }
    std::string sfoBytes;
    std::string sfoVer;
    if (!readAll(src, sfoBytes) || (sfoVer = appVerFromPkg(sfoBytes)).empty()) {
        r.status = LocalUpdateStatus::Failed;
        r.detail = "SFO illeggibile";
        updWarn("SFO illeggibile %s", src.c_str());
        return r;
    }
    PkgVer got = parsePkgVer(sfoVer);
    PkgVer cur = parsePkgVer(current);
    updLog("SFO %s corrente %s", sfoVer.c_str(), current.c_str());
    if (!got.ok || (cur.ok && cmpPkgVer(got, cur) <= 0)) {
        r.status = LocalUpdateStatus::Failed;
        r.detail = "versione non più nuova";
        updWarn("versione SFO %s non più nuova di %s", sfoVer.c_str(), current.c_str());
        return r;
    }

    if (!ensureDir(kPkgDirLegacy)) updWarn("mkdir %s non riuscito, provo la copia lo stesso", kPkgDirLegacy);
    std::string dest = std::string(kPkgDirLegacy) + "/" + name;
    if (src != dest) {
        updLog("copio %s → %s (%lld byte)", src.c_str(), dest.c_str(), placed.lseekBytes);
        if (!copyFile(src, dest) || !fsyncPath(dest)) {
            r.status = LocalUpdateStatus::Failed;
            r.detail = "copia in /data/pkg fallita";
            updWarn("copia fallita %s", dest.c_str());
            return r;
        }
        Measured copied{};
        if (!waitStableSize(dest, copied) || !copied.magic || copied.lseekBytes != placed.lseekBytes) {
            r.status = LocalUpdateStatus::Failed;
            r.detail = "copia non verificata";
            updWarn("copia non verificata %s magic %d size %lld (attesi %lld)", dest.c_str(), copied.magic ? 1 : 0,
                    copied.lseekBytes, placed.lseekBytes);
            return r;
        }
        if (unlink(src.c_str()) == 0) updLog("rimosso sorgente %s", src.c_str());
        else updWarn("sorgente non rimossa %s (%d)", src.c_str(), errno);
    } else {
        updLog("pkg già in /data/pkg: %s", dest.c_str());
    }
    unlinkOthers(kPkgDirLegacy, name);
    r.path = dest;

    bool handed = false;
    if (installViaGoldhenRpi(dest, name)) {
        handed = true;
        updLog("RPI ha preso il pkg");
    } else if (sendBinloaderPayload()) {
        handed = true;
        updLog("BinLoader ha preso il payload");
    }
    if (!handed) {
        r.status = LocalUpdateStatus::Failed;
        r.detail = std::string(kManualInstall) + " (" + dest + ")";
        updWarn("%s", r.detail.c_str());
        platform::notify(kManualInstall, std::string("[UPDATE] ") + r.detail);
        return r;
    }

    writePending(ver);
    updLog("install affidata a GoldHEN %s — chiudo l'app", ver.c_str());
    platform::notify("Aggiornamento in corso, riapri PSBox tra poco", "[UPDATE] goldhen ok");
    platform::sleepMs(400);
    r.status = LocalUpdateStatus::Installed;
    stampEnter("exitAfterUpdate");
    (void)exitAfterUpdate();
    stampLeave("exitAfterUpdate", -1);
    updWarn("LoadExec exit non ha preso — _exit");
    xc_log_stamp("[UPDATE] _exit dopo install");
    _exit(0);
    return r;
}

}  // namespace xc::app

