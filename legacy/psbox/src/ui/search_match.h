// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Confronto della ricerca: nome, title id e product id. Fortnite sul cloud
// ha title id "FORTNITE" e product id BT5P2X999VH2: entrambi devono trovarlo.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace xc::ui {

inline std::string foldSearch(const std::string& s) {
    static const char kLatin1[] = "aaaaaaaceeeeiiiidnooooo ouuuuytsaaaaaaaceeeeiiiidnooooo ouuuuyty";
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = ' ';
        size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
        }
        i += n;
        char ch = ' ';
        if (cp < 0x80 && std::isalnum(static_cast<int>(cp))) ch = static_cast<char>(std::tolower(static_cast<int>(cp)));
        else if (cp >= 0xC0 && cp <= 0xFF) ch = kLatin1[cp - 0xC0];
        out += ch;
    }
    return out;
}

inline bool isFortniteTile(const std::string& titleId, const std::string& productId, const std::string& nameFolded) {
    return titleId == "FORTNITE" || productId == "BT5P2X999VH2" || nameFolded == "fortnite";
}

// Testo su cui si cercano le parole: nome, title id, product id, e per
// Fortnite anche gli alias (il catalogo a volte lascia il nome vuoto o un
// pack più corto con lo stesso title id).
inline std::string searchHaystack(const std::string& name, const std::string& titleId, const std::string& productId) {
    std::string nameF = foldSearch(name);
    std::string hay = nameF + " " + foldSearch(titleId) + " " + foldSearch(productId);
    if (isFortniteTile(titleId, productId, nameF)) hay += " fortnite bt5p2x999vh2";
    return hay;
}

inline std::string withoutSearchSpaces(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != ' ') out += c;
    return out;
}

inline bool searchMatches(const std::string& hay, const std::vector<std::string>& words, const std::string& compact) {
    if (words.empty()) return true;
    bool all = true;
    for (const auto& w : words)
        if (hay.find(w) == std::string::npos) all = false;
    if (all) return true;
    if (compact.empty()) return false;
    return withoutSearchSpaces(hay).find(compact) != std::string::npos;
}

}  // namespace xc::ui
