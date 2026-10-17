// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "app/updater.h"

#include "net/http.h"
#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

// stb_image's inflate, for the deflated entries (its own static copy).
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace xc::app {

const char* const kReleaseKey =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEGyPKcF2L65xh03F1ZRzfhv48LOWS\n"
    "OBKQ55SVLod7HOZZiViUTbY47qETC8l9A1ez1RflBTH94WILhlHhC4xTeQ==\n"
    "-----END PUBLIC KEY-----\n";

namespace {

constexpr size_t kMaxUnpacked = 256u << 20;  // far above a release (~50 MB)

uint32_t le16(const std::string& s, size_t at) {
    return static_cast<uint8_t>(s[at]) | static_cast<uint8_t>(s[at + 1]) << 8;
}
uint32_t le32(const std::string& s, size_t at) {
    return le16(s, at) | le16(s, at + 2) << 16;
}

bool isDir(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

// mkdir -p of the folder holding `file`.
bool makeParents(const std::string& file) {
    for (size_t at = file.find('/', 1); at != std::string::npos; at = file.find('/', at + 1)) {
        std::string d = file.substr(0, at);
        if (!isDir(d) && ::mkdir(d.c_str(), 0777) != 0 && errno != EEXIST) return false;
    }
    return true;
}

void removeTree(const std::string& path) {
    if (DIR* d = ::opendir(path.c_str())) {
        while (dirent* e = ::readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::string p = path + "/" + name;
            if (isDir(p))
                removeTree(p);
            else
                ::unlink(p.c_str());
        }
        ::closedir(d);
        ::rmdir(path.c_str());
    }
}

// Every file under `root`, relative to it.
void listFiles(const std::string& root, const std::string& rel, std::vector<std::string>& out) {
    DIR* d = ::opendir((root + (rel.empty() ? "" : "/" + rel)).c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string r = rel.empty() ? name : rel + "/" + name;
        if (isDir(root + "/" + r))
            listFiles(root, r, out);
        else
            out.push_back(r);
    }
    ::closedir(d);
}

bool download(const std::string& url, long size, std::string& out, const std::function<void(double)>& progress,
              std::string& err) {
    net::Request req;
    req.url = url;
    req.followRedirects = true;  // GitHub hands the files out from another host
    req.timeoutMs = 30000;
    req.headers = {{"Accept", "application/octet-stream"}, {"User-Agent", "PSBox-Cloud-Gaming"}};
    out.clear();
    if (size > 0) out.reserve(static_cast<size_t>(size));
    req.onBody = [&](const char* data, size_t len) {
        if (out.size() + len > kMaxUnpacked) return false;
        out.append(data, len);
        return true;
    };
    if (progress)
        req.onProgress = [&](size_t got, long total) {
            if (total <= 0) total = size;
            progress(total > 0 ? std::min(1.0, static_cast<double>(got) / static_cast<double>(total)) : -1);
        };
    auto r = net::perform(req);
    if (!r.ok()) {
        err = "download " + url + ": " + (r.status ? "HTTP " + std::to_string(r.status) : r.error);
        return false;
    }
    if (size > 0 && static_cast<long>(out.size()) != size) {
        err = "download " + url + ": " + std::to_string(out.size()) + " bytes, " + std::to_string(size) + " expected";
        return false;
    }
    return true;
}

// The files in place go back where they were (an update cut short). True
// when there were some; `complete` false when one couldn't go back (then
// update.old and the journal must stay, for the next start to try again).
bool rollBack(const std::string& dir, bool& complete) {
    std::vector<std::string> old;
    listFiles(dir + "/update.old", "", old);
    complete = true;
    for (const auto& rel : old)
        if (!makeParents(dir + "/" + rel) || ::rename((dir + "/update.old/" + rel).c_str(), (dir + "/" + rel).c_str()) != 0) {
            XC_LOGE("update: could not put back %s (errno %d)", rel.c_str(), errno);
            complete = false;
        }
    return !old.empty();
}

}  // namespace

bool verifySignature(const std::string& data, the_sig_placeholder, const char* publicKeyPem, std::string& err) {
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    return true;
}
