/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/auth/release_check.h"
#include "../src/auth/http_client.h"
#include "../src/ui/screen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host stubs for http client in tests */
X4Http *x4_http_open(int *err) { if (err) *err = 0; return NULL; }
void x4_http_close(X4Http *h) { (void)h; }
int x4_http_releases_request(X4Http *h, const char *url, char *body, size_t cap,
    size_t *len, int *status, const _Atomic int *cancel)
{
    (void)h; (void)url; (void)body; (void)cap; (void)len; (void)status; (void)cancel;
    return -1;
}

static void test_version_parsing(void)
{
    int maj = 0, min = 0, pat = 0;

    assert(x4_version_parse("1.1.0-beta", &maj, &min, &pat) == 0);
    assert(maj == 1 && min == 1 && pat == 0);

    assert(x4_version_parse("v1.1.1-beta", &maj, &min, &pat) == 0);
    assert(maj == 1 && min == 1 && pat == 1);

    assert(x4_version_parse("lab-0.7.55", &maj, &min, &pat) == 0);
    assert(maj == 0 && min == 7 && pat == 55);

    assert(x4_version_parse("0.7.56", &maj, &min, &pat) == 0);
    assert(maj == 0 && min == 7 && pat == 56);

    assert(x4_version_parse("0.7.55-lab", &maj, &min, &pat) == 0);
    assert(maj == 0 && min == 7 && pat == 55);

    assert(x4_version_parse("invalid", &maj, &min, &pat) != 0);
    assert(x4_version_parse("", &maj, &min, &pat) != 0);
    assert(x4_version_parse(NULL, &maj, &min, &pat) != 0);
    printf("PASS test_version_parsing\n");
}

static void test_version_comparison(void)
{
    /* 1.1.0-beta < 1.1.1-beta */
    assert(x4_version_compare("1.1.0-beta", "1.1.1-beta") < 0);
    assert(x4_version_compare("1.1.1-beta", "1.1.0-beta") > 0);

    /* 1.1.0 = 1.1.0-beta */
    assert(x4_version_compare("1.1.0", "1.1.0-beta") == 0);
    assert(x4_version_compare("v1.1.0", "1.1.0-beta") == 0);

    /* 0.7.55-lab < 0.7.56 */
    assert(x4_version_compare("0.7.55-lab", "0.7.56") < 0);
    assert(x4_version_compare("0.7.56", "0.7.55-lab") > 0);

    /* Lab prefix equality */
    assert(x4_version_compare("lab-0.7.55", "0.7.55-lab") == 0);

    /* Additional comparison checks */
    assert(x4_version_compare("1.2.0", "1.1.9") > 0);
    assert(x4_version_compare("2.0.0", "1.99.99") > 0);
    assert(x4_version_compare("0.7.55", "0.7.55") == 0);
    printf("PASS test_version_comparison\n");
}

static void test_json_tag_extraction(void)
{
    /* 1. Public JSON single object */
    const char *pub_json =
        "{\n"
        "  \"url\": \"https://api.github.com/repos/asp11edgr/XCloud4/releases/123\",\n"
        "  \"tag_name\": \"v1.1.1-beta\",\n"
        "  \"name\": \"1.1.1-beta\",\n"
        "  \"draft\": false\n"
        "}";
    char tag[64] = {0};
    char ver[32] = {0};
    assert(x4_release_parse_tag(pub_json, strlen(pub_json), true, tag, sizeof(tag), ver, sizeof(ver)) == 0);
    assert(strcmp(tag, "v1.1.1-beta") == 0);
    assert(strcmp(ver, "1.1.1-beta") == 0);

    /* 2. Lab JSON array */
    const char *lab_json =
        "[\n"
        "  {\"tag_name\": \"v1.1.0\", \"name\": \"v1.1.0\"},\n"
        "  {\"tag_name\": \"lab-0.7.56\", \"name\": \"lab-0.7.56\"},\n"
        "  {\"tag_name\": \"lab-0.7.55\", \"name\": \"lab-0.7.55\"}\n"
        "]";
    memset(tag, 0, sizeof(tag));
    memset(ver, 0, sizeof(ver));
    assert(x4_release_parse_tag(lab_json, strlen(lab_json), false, tag, sizeof(tag), ver, sizeof(ver)) == 0);
    assert(strcmp(tag, "lab-0.7.56") == 0);
    assert(strcmp(ver, "0.7.56") == 0);

    /* 3. Lab JSON array with no lab releases */
    const char *lab_json_nolab =
        "[\n"
        "  {\"tag_name\": \"v1.1.0\", \"name\": \"v1.1.0\"}\n"
        "]";
    assert(x4_release_parse_tag(lab_json_nolab, strlen(lab_json_nolab), false, tag, sizeof(tag), ver, sizeof(ver)) == -1);

    /* 4. Corrupt / empty JSON */
    assert(x4_release_parse_tag("", 0, true, tag, sizeof(tag), ver, sizeof(ver)) == -1);
    assert(x4_release_parse_tag("{corrupt", 8, true, tag, sizeof(tag), ver, sizeof(ver)) == -1);
    assert(x4_release_parse_tag("{}", 2, true, tag, sizeof(tag), ver, sizeof(ver)) == -1);
    assert(x4_release_parse_tag("[]", 2, false, tag, sizeof(tag), ver, sizeof(ver)) == -1);
    assert(x4_release_parse_tag("not json", 8, false, tag, sizeof(tag), ver, sizeof(ver)) == -1);
    printf("PASS test_json_tag_extraction\n");
}

static void test_qr_payload(void)
{
    char payload[512] = {0};

    /* Valid cases */
    assert(x4_auth_qr_payload("https://www.microsoft.com/link", "ABCD-1234", payload, sizeof(payload)));
    assert(strcmp(payload, "https://www.microsoft.com/link?otc=ABCD-1234") == 0);

    assert(x4_auth_qr_payload("https://microsoft.com/devicelogin", "WXYZ-9876", payload, sizeof(payload)));
    assert(strcmp(payload, "https://microsoft.com/devicelogin?otc=WXYZ-9876") == 0);

    /* Invalid URIs */
    assert(!x4_auth_qr_payload("https://attacker.com/link", "ABCD-1234", payload, sizeof(payload)));
    assert(!x4_auth_qr_payload("http://www.microsoft.com/link", "ABCD-1234", payload, sizeof(payload)));
    assert(!x4_auth_qr_payload(NULL, "ABCD-1234", payload, sizeof(payload)));

    /* Invalid codes */
    assert(!x4_auth_qr_payload("https://www.microsoft.com/link", "ABCD 1234", payload, sizeof(payload)));
    assert(!x4_auth_qr_payload("https://www.microsoft.com/link", "", payload, sizeof(payload)));
    assert(!x4_auth_qr_payload("https://www.microsoft.com/link", "ABCD;inject", payload, sizeof(payload)));
    assert(!x4_auth_qr_payload("https://www.microsoft.com/link", NULL, payload, sizeof(payload)));

    printf("PASS test_qr_payload\n");
}

int main(void)
{
    test_version_parsing();
    test_version_comparison();
    test_json_tag_extraction();
    test_qr_payload();
    printf("ALL TESTS PASSED in test_update_qr\n");
    return 0;
}
