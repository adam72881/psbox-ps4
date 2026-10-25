// g++ -std=c++20 -I src -o /tmp/update_pick_test tests/update_pick_test.cpp
#include "app/update_pick.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail;

static void expect(const char* name, bool cond) {
    if (cond) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    ++g_fail;
}

int main() {
    using xc::app::orbisStatSize;
    using xc::app::parsePkgVer;
    using xc::app::pkgNameVsInstalled;
    using xc::app::pkgVerText;

    const char* cmp = pkgNameVsInstalled("PSBox-PS4-01.16.pkg", "1.0.15");
    std::printf("PSBox-PS4-01.16.pkg vs 1.0.15: %s\n", cmp);
    expect("01.16 piu nuova", std::strcmp(cmp, "più nuova") == 0);
    expect("01.17 dalla 01.16", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.17.pkg", "1.0.16-ps4"), "più nuova") == 0);
    expect("01.18 dalla 01.17", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.18.pkg", "1.0.17-ps4"), "più nuova") == 0);
    expect("01.19 dalla 01.18", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.19.pkg", "1.0.18-ps4"), "più nuova") == 0);
    expect("01.20 dalla 01.19", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.20.pkg", "1.0.19-ps4"), "più nuova") == 0);
    expect("01.21 dalla 01.20", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.21.pkg", "1.0.20-ps4"), "più nuova") == 0);
    expect("01.22 dalla 01.20", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.22.pkg", "1.0.20-ps4"), "più nuova") == 0);
    expect("01.22 dalla 01.21", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.22.pkg", "1.0.21-ps4"), "più nuova") == 0);
    expect("01.23 dalla 01.22", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.23.pkg", "1.0.22-ps4"), "più nuova") == 0);
    expect("01.24 dalla 01.23", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.24.pkg", "1.0.23-ps4"), "più nuova") == 0);
    expect("01.25 dalla 01.24", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.25.pkg", "1.0.24-ps4"), "più nuova") == 0);
    expect("01.26 dalla 01.25", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.26.pkg", "1.0.25-ps4"), "più nuova") == 0);
    expect("01.27 dalla 01.26", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.27.pkg", "1.0.26-ps4"), "più nuova") == 0);
    expect("01.28 dalla 01.27", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.28.pkg", "1.0.27-ps4"), "più nuova") == 0);
    expect("01.29 dalla 01.28", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.29.pkg", "1.0.28-ps4"), "più nuova") == 0);
    expect("01.30 dalla 01.29", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.30.pkg", "1.0.29-ps4"), "più nuova") == 0);
    expect("01.31 dalla 01.30", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.31.pkg", "1.0.30-ps4"), "più nuova") == 0);
    expect("01.33 dalla 01.32", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.33.pkg", "1.0.32-ps4"), "più nuova") == 0);
    expect("01.34 dalla 01.33", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.34.pkg", "1.0.33-ps4"), "più nuova") == 0);
    expect("01.17 nome", xc::app::isPkgFileName("PSBox-PS4-01.17.pkg"));
    expect("01.17 testo", pkgVerText(parsePkgVer("PSBox-PS4-01.17.pkg")) == "1.0.17");
    expect("nome pkg", xc::app::isPkgFileName("PSBox-PS4-01.16.pkg"));
    expect("nome 4.1 no", pkgVerText(parsePkgVer("PSBox-PS4-01.16.pkg")) == "1.0.16");

    expect("01.15 non piu nuova", std::strcmp(pkgNameVsInstalled("PSBox-PS4-01.15.pkg", "1.0.15-ps4"), "non più nuova") == 0);
    expect("app ver 01.16", pkgVerText(parsePkgVer("01.16")) == "1.0.16");
    expect("tre numeri", pkgVerText(parsePkgVer("1.0.16-ps4")) == "1.0.16");

    unsigned char raw[0x60] = {};
    uint64_t real = 9764864;
    uint64_t blocks = 19136;
    std::memcpy(raw + 0x48, &real, 8);
    std::memcpy(raw + 0x50, &blocks, 8);
    expect("stat 0x48", orbisStatSize(raw, sizeof raw) == 9764864);
    uint64_t wrong = 0;
    std::memcpy(&wrong, raw + 0x50, 8);
    expect("offset musl 0x50", wrong == 19136);

    unsigned char mag[4] = {0x7f, 0x43, 0x4e, 0x54};
    expect("magic", xc::app::pkgMagicOk(mag));
    mag[3] = 0;
    expect("magic no", !xc::app::pkgMagicOk(mag));

    if (g_fail) {
        std::fprintf(stderr, "%d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("update_pick: ok\n");
    return 0;
}
