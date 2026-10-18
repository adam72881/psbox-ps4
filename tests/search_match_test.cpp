// g++ -std=c++20 -I src -o /tmp/search_match_test tests/search_match_test.cpp
#include "ui/search_match.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail;

static void expect(const char* name, bool cond) {
    if (cond) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    ++g_fail;
}

int main() {
    using xc::ui::foldSearch;
    using xc::ui::isFortniteTile;
    using xc::ui::searchHaystack;
    using xc::ui::searchMatches;

    expect("fold fortnite", foldSearch("Fortnite") == "fortnite");
    expect("fold id", foldSearch("FORTNITE") == "fortnite");
    expect("fold product", foldSearch("BT5P2X999VH2") == "bt5p2x999vh2");
    expect("è fortnite per id", isFortniteTile("FORTNITE", "", ""));
    expect("è fortnite per product", isFortniteTile("", "BT5P2X999VH2", ""));
    expect("è fortnite per nome", isFortniteTile("", "", "fortnite"));
    expect("non è un altro gioco", !isFortniteTile("HALO", "9N201KQXS5BM", "halo"));

    std::string hay = searchHaystack("Fortnite", "FORTNITE", "BT5P2X999VH2");
    std::vector<std::string> words{"fortnite"};
    expect("nome", searchMatches(hay, words, "fortnite"));
    words = {"bt5p2x999vh2"};
    expect("product id", searchMatches(hay, words, "bt5p2x999vh2"));

    // Pack più corto, stesso title id: la ricerca «fortnite» e l'id restano.
    hay = searchHaystack("OG", "FORTNITE", "9ZZZZZZZZZZZ");
    words = {"fortnite"};
    expect("alias da title id", searchMatches(hay, words, "fortnite"));
    words = {"bt5p2x999vh2"};
    expect("alias product anche se il pack ha un altro id di vetrina", searchMatches(hay, words, "bt5p2x999vh2"));

    hay = searchHaystack("Halo Infinite", "HALO", "9N201KQXS5BM");
    words = {"fortnite"};
    expect("halo non è fortnite", !searchMatches(hay, words, "fortnite"));
    words = {"halo"};
    expect("halo per nome", searchMatches(hay, words, "halo"));

    if (g_fail) {
        std::fprintf(stderr, "%d fail\n", g_fail);
        return 1;
    }
    std::printf("search_match ok\n");
    return 0;
}
