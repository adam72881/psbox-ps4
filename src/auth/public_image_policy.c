/* SPDX-License-Identifier: GPL-3.0-only */
#include "http_client.h"
#include <string.h>
static size_t image_bounded_length(const char *p, size_t cap) { size_t n=0; while(n<cap && p[n]) ++n; return n; }
static int image_alphanumeric(unsigned char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9'); }
int x4_http_public_image_url(const char *url)
{
    static const char prefix[] = "https://store-images.s-microsoft.com/image/apps.";
    static const char suffix[] = "?w=512&q=80&format=jpg";
    if (!url) return 0;
    size_t n = image_bounded_length(url, 513);
    size_t p = sizeof(prefix) - 1, s = sizeof(suffix) - 1;
    if (n > 512 || n <= p + s || memcmp(url, prefix, p) ||
        memcmp(url + n - s, suffix, s)) return 0;
    for (size_t i = p; i < n - s; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (!image_alphanumeric(c) && c != '.' && c != '-' && c != '_') return 0;
    }
    /* No slash, escaped path, userinfo, port, query override or dot segment. */
    return 1;
}

