/* SPDX-License-Identifier: GPL-3.0-or-later
 * Versione di un PSBox-PS4-*.pkg e dimensione letta dal layout stat di Orbis.
 * Il struct stat di musl mette st_size a 0x50. Sul kernel FreeBSD/amd64 della
 * PS4 st_size è un int64 a 0x48, dopo atim/mtim/ctim: a 0x50 ci sono i blocchi. */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace xc::app {

struct PkgVer {
    int a = 0, b = 0, c = 0;
    bool ok = false;
};

inline int cmpPkgVer(PkgVer x, PkgVer y) {
    if (x.a != y.a) return x.a < y.a ? -1 : 1;
    if (x.b != y.b) return x.b < y.b ? -1 : 1;
    if (x.c != y.c) return x.c < y.c ? -1 : 1;
    return 0;
}

// "1.0.15-ps4" -> 1.0.15. "01.16" e "PSBox-PS4-01.16.pkg" -> 1.0.16, non 1.16.
inline PkgVer parsePkgVer(std::string s) {
    PkgVer v;
    auto cut = s.find("-ps4");
    if (cut != std::string::npos) s.resize(cut);
    static const char kPrefix[] = "PSBox-PS4-";
    if (s.size() >= sizeof kPrefix - 1 && s.compare(0, sizeof kPrefix - 1, kPrefix) == 0)
        s.erase(0, sizeof kPrefix - 1);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".pkg") == 0) s.resize(s.size() - 4);
    int n = 0, part[3] = {};
    for (size_t i = 0; i < s.size() && n < 3;) {
        if (s[i] < '0' || s[i] > '9') {
            ++i;
            continue;
        }
        int x = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') x = x * 10 + (s[i++] - '0');
        part[n++] = x;
        if (i < s.size() && s[i] == '.') ++i;
    }
    if (n == 3) {
        v.a = part[0];
        v.b = part[1];
        v.c = part[2];
        v.ok = true;
    } else if (n == 2) {
        v.a = part[0];
        v.b = 0;
        v.c = part[1];
        v.ok = true;
    }
    return v;
}

inline std::string pkgVerText(PkgVer v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d.%d.%d", v.a, v.b, v.c);
    return buf;
}

inline bool isPkgFileName(const std::string& raw) {
    std::string name = raw;
    while (!name.empty() && (name.back() == '\0' || name.back() == ' ' || name.back() == '/')) name.pop_back();
    static const char kPrefix[] = "psbox-ps4-";
    constexpr size_t kPrefixLen = sizeof kPrefix - 1;  // 10, non 11
    if (name.size() < kPrefixLen + 4) return false;
    auto lower = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
    };
    for (size_t i = 0; i < kPrefixLen; ++i)
        if (lower(static_cast<unsigned char>(name[i])) != kPrefix[i]) return false;
    for (int i = 0; i < 4; ++i)
        if (lower(static_cast<unsigned char>(name[name.size() - 4 + static_cast<size_t>(i)])) != ".pkg"[i]) return false;
    return true;
}

inline long long orbisStatSize(const unsigned char* b, size_t n) {
    if (!b || n < 0x50) return -1;
    uint64_t v = 0;
    std::memcpy(&v, b + 0x48, 8);
    return static_cast<long long>(v);
}

inline bool pkgMagicOk(const unsigned char* m) {
    return m && m[0] == 0x7f && m[1] == 0x43 && m[2] == 0x4e && m[3] == 0x54;
}

// Confronto sul solo nome. "più nuova" se il file supera la versione installata.
inline const char* pkgNameVsInstalled(const char* fileName, const char* installed) {
    PkgVer file = parsePkgVer(fileName ? fileName : "");
    PkgVer cur = parsePkgVer(installed ? installed : "");
    if (!file.ok) return "versione illeggibile";
    if (!cur.ok) return "versione installata illeggibile";
    if (cmpPkgVer(file, cur) > 0) return "più nuova";
    return "non più nuova";
}

}  // namespace xc::app
