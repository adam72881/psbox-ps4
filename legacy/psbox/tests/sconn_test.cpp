// g++ -std=c++20 -I src -o /tmp/sconn_test tests/sconn_test.cpp
#include "platform/ps4/sconn_fix.h"

#include <cstdio>
#include <cstring>

static int g_fail;

static void expect(const char* name, bool cond) {
    if (cond) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    ++g_fail;
}

int main() {
    unsigned char linux_sa[16] = {};
    linux_sa[0] = 123;
    linux_sa[1] = 0;
    linux_sa[2] = 0x13;
    linux_sa[3] = 0x88;
    linux_sa[8] = 0x11;
    linux_sa[9] = 0x22;
    expect("linux", xc_sconn_fix(linux_sa, 16) == 1);
    expect("len", linux_sa[0] == 16);
    expect("fam", linux_sa[1] == 123);
    expect("porta", linux_sa[2] == 0x13 && linux_sa[3] == 0x88);
    expect("ptr", linux_sa[8] == 0x11 && linux_sa[9] == 0x22);

    unsigned char bsd[16] = {16, 123, 0x01, 0x02};
    expect("bsd gia", xc_sconn_fix(bsd, 16) == 0);
    expect("bsd len", bsd[0] == 16 && bsd[1] == 123 && bsd[2] == 0x01);

    unsigned char inet[16] = {16, 2, 0, 0};
    expect("inet", xc_sconn_fix(inet, 16) == 0 && inet[0] == 16 && inet[1] == 2);

    if (g_fail) {
        std::fprintf(stderr, "%d falliti\n", g_fail);
        return 1;
    }
    std::puts("sconn ok");
    return 0;
}
