// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Layer PS4 (GoldHEN / OpenOrbis). Stessa interfaccia di platform_ps5.cpp:
// cartella dati, DNS via sceNetResolver, notifiche, riavvio dell'eboot.
#include "platform/platform.h"
#include "util/log.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>

extern "C" int ps4_random_bytes(void* out, size_t len);

namespace xc::platform {

namespace {
constexpr const char* kDataDir = "/data/psbox";
std::string g_dataDir = kDataDir;
std::string g_dataNote;
std::atomic<int> g_netPool{-1};

bool writable(const std::string& dir) {
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0 && ::mkdir(dir.c_str(), 0777) != 0) return false;
    std::string probe = dir + "/.write-test";
    if (!writeFileAtomic(probe, "ok")) return false;
    ::unlink(probe.c_str());
    return true;
}
}  // namespace

bool init() {
    if (writable(kDataDir)) {
        g_dataDir = kDataDir;
    } else {
        g_dataDir = "/data";
        g_dataNote = std::string(kDataDir) + " non scrivibile (errno " + std::to_string(errno) + "); uso /data";
        if (!writable(g_dataDir)) g_dataNote += " — neanche /data è scrivibile: il login non resta salvato";
    }

    int mod = static_cast<int>(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET));
    if (mod < 0) XC_LOGW("sceSysmoduleLoadModuleInternal(NET): 0x%08x", static_cast<unsigned>(mod));
    int rc = sceNetInit();
    if (rc < 0) XC_LOGW("sceNetInit: 0x%08x", static_cast<unsigned>(rc));
    int pool = sceNetPoolCreate("psbox", 256 * 1024, 0);
    if (pool < 0) XC_LOGE("sceNetPoolCreate: 0x%08x (i nomi non si risolvono)", static_cast<unsigned>(pool));
    g_netPool = pool;

    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    rc = sceNetCtlInit();
    if (rc < 0) XC_LOGW("sceNetCtlInit: 0x%08x", static_cast<unsigned>(rc));
    /* Un ICMP unreachable sul socket UDP di ICE, con SIGPIPE, chiude il processo. */
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}

void shutdown() {}

std::string dataDir() { return g_dataDir; }
std::string dataDirNote() { return g_dataNote; }
std::string appDir() { return "/app0"; }
std::string assetDir() { return "/app0/assets"; }
std::string caBundlePath() { return "/app0/assets/cacert.pem"; }

bool randomBytes(void* out, size_t len) { return ps4_random_bytes(out, len) != 0; }

bool resolveIPv4(const std::string& host, uint32_t& addr, std::string& err) {
    in_addr literal{};
    if (::inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        addr = literal.s_addr;
        return true;
    }
    int pool = g_netPool.load();
    if (pool < 0) {
        err = "DNS non inizializzato per " + host;
        return false;
    }
    int rid = sceNetResolverCreate("psbox", pool, 0);
    if (rid < 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "sceNetResolverCreate 0x%08x", static_cast<unsigned>(rid));
        err = "DNS fallito per " + host + ": " + buf;
        return false;
    }
    OrbisNetInAddr a{};
    int rc = sceNetResolverStartNtoa(rid, host.c_str(), &a, 0, 0, 0);
    sceNetResolverDestroy(rid);
    if (rc < 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "0x%08x", static_cast<unsigned>(rc));
        err = "DNS fallito per " + host + " (" + buf + ")";
        return false;
    }
    addr = a.s_addr;
    return true;
}

bool restartApp() {
    const char* argv[] = {nullptr};
    int rc = sceSystemServiceLoadExec("/app0/eboot.bin", argv);
    XC_LOGE("riavvio: sceSystemServiceLoadExec ha risposto 0x%08x", static_cast<unsigned>(rc));
    return false;
}

std::string firmwareVersion() {
    OrbisKernelSwVersion v{};
    v.Size = sizeof v;
    if (sceKernelGetSystemSwVersion(&v) != 0) return {};
    v.VersionString[sizeof v.VersionString - 1] = 0;
    std::string s = v.VersionString;
    while (!s.empty() && (s.front() == ' ' || s.front() == '0')) s.erase(0, 1);
    return s;
}

void notify(const std::string& text) { notify(text, text); }

void notify(const std::string& text, const std::string& forLog) {
    XC_LOGI("NOTIFY: %s", forLog.c_str());
    OrbisNotificationRequest req{};
    req.type = NotificationRequest;
    std::snprintf(req.message, sizeof req.message, "%s", text.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

bool systemKeyboardAvailable() { return false; }

bool openSystemKeyboard(const std::string&, const std::string&, size_t, KeyboardKind) { return false; }

KeyboardStatus pollSystemKeyboard(std::string&) { return KeyboardStatus::Closed; }

}  // namespace xc::platform

extern "C" int xc_resolve_ipv4(const char* host, uint32_t* addr) {
    std::string err;
    if (!addr) return -1;
    if (xc::platform::resolveIPv4(host ? host : "", *addr, err)) return 0;
    return -1;
}
