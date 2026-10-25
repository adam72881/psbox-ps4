/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile the complete production HTTP implementation against a deterministic
 * native-ABI fixture. This validates local contracts, not PS4 networking. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../src/auth/http_client.c"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "HTTP check failed %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum ResourceKind { NONE, TEMPLATE, CONNECTION, REQUEST };
static struct Resource { enum ResourceKind kind; int parent; const char *borrowed_url; bool keepalive;
    unsigned verify; int redirect, cookies; size_t offset; } resource[2048];
static unsigned next_id, connections, templates, requests, sends, terminated;
static unsigned public_connections, private_connections;
static bool fail_send, fail_read, fail_delete_request, fail_delete_connection;
static bool missing_header_api;
static int response_status = 200;
static const char *response_body = "{}", *response_headers = "HTTP/1.1 200 OK\r\n\r\n";
static uint64_t fake_clock;
static _Atomic int *cancel_during_send;

uint64_t sceKernelGetProcessTime(void) { return fake_clock; }
static int32_t new_resource(enum ResourceKind kind, int parent)
{
    CHECK(++next_id < sizeof(resource) / sizeof(resource[0]));
    resource[next_id] = (struct Resource){.kind = kind, .parent = parent, .redirect = -1, .cookies = -1};
    return (int32_t)next_id;
}
static int32_t mock_zero(void) { return 0; }
static int32_t mock_pool(const char *name, int32_t size, int32_t flags)
{ CHECK(!strcmp(name, "x4-http-net") && size == X4_NET_POOL_SIZE && !flags); return 51; }
static int32_t mock_pool_destroy(int32_t id) { CHECK(id == 51); return 0; }
static int32_t mock_ssl(size_t bytes) { CHECK(bytes == X4_SSL_POOL_SIZE); return 52; }
static int32_t mock_ssl_term(int32_t id) { CHECK(id == 52); return 0; }
static int32_t mock_http(int32_t net, int32_t ssl, size_t bytes)
{ CHECK(net == 51 && ssl == 52 && bytes == X4_HTTP_POOL_SIZE); return 53; }
static int32_t mock_http_term(int32_t id)
{ CHECK(id == 53); for (unsigned i = 1; i <= next_id; ++i) CHECK(resource[i].kind == NONE); ++terminated; return 0; }
static int32_t mock_template(int32_t ctx, const char *agent, int32_t version, int32_t proxy)
{ CHECK(ctx == 53 && !strcmp(agent, "XCloud4") && version == 2 && !proxy); ++templates; return new_resource(TEMPLATE, 0); }
static int32_t mock_timeout(int32_t tmpl, uint32_t timeout)
{ CHECK(resource[tmpl].kind == TEMPLATE && timeout == X4_HTTP_TIMEOUT_USEC); return 0; }
static int32_t mock_redirect(int32_t tmpl, int32_t on)
{ CHECK(resource[tmpl].kind == TEMPLATE && !on); resource[tmpl].redirect = on; return 0; }
static int32_t mock_cookie(int32_t tmpl, int32_t on)
{ CHECK(resource[tmpl].kind == TEMPLATE && !on); resource[tmpl].cookies = on; return 0; }
static int32_t mock_tls(int32_t tmpl, uint32_t flags)
{ CHECK(resource[tmpl].kind == TEMPLATE && flags == X4_HTTPS_VERIFY_ALL); resource[tmpl].verify = flags; return 0; }
static int32_t mock_connection(int32_t tmpl, const char *url, bool keepalive)
{
    CHECK(resource[tmpl].kind == TEMPLATE && resource[tmpl].verify == X4_HTTPS_VERIFY_ALL &&
        !resource[tmpl].redirect && !resource[tmpl].cookies);
    if (keepalive) {
        CHECK(!strncmp(url, store_prefix, sizeof(store_prefix) - 1) || x4_http_public_image_url(url));
        ++public_connections;
    } else ++private_connections;
    ++connections;
    int id = new_resource(CONNECTION, tmpl);
    resource[id].borrowed_url = url;
    resource[id].keepalive = keepalive;
    return id;
}
static int32_t mock_request(int32_t conn, int32_t method, const char *url, uint64_t size)
{
    CHECK(resource[conn].kind == CONNECTION && resource[conn].borrowed_url[0]);
    if (resource[conn].keepalive) CHECK(method == X4_HTTP_METHOD_GET && !size);
    ++requests;
    int id = new_resource(REQUEST, conn);
    resource[id].borrowed_url = url;
    return id;
}
static int32_t mock_header(int32_t req, const char *name, const char *value, uint32_t mode)
{
    CHECK(resource[req].kind == REQUEST && mode == X4_HTTP_HEADER_OVERWRITE);
    if (resource[resource[req].parent].keepalive) {
        CHECK(!strcmp(name, "Accept") || !strcmp(name, "Accept-Encoding"));
        CHECK(!strstr(value, "Bearer") && !strstr(value, "credential"));
    }
    return 0;
}
static int32_t mock_send(int32_t req, const void *post, size_t size)
{
    CHECK(resource[req].kind == REQUEST && resource[req].borrowed_url[0]);
    if (resource[resource[req].parent].keepalive) CHECK(!post && !size);
    ++sends;
    if (cancel_during_send) atomic_store(cancel_during_send, 1);
    if (fail_send) { fail_send = false; return -77; }
    return 0;
}
static int32_t mock_status(int32_t req, int32_t *status)
{ CHECK(resource[req].kind == REQUEST); *status = response_status; return 0; }
static int32_t mock_headers(int32_t req, char **headers, size_t *size)
{ CHECK(resource[req].kind == REQUEST); *headers = (char *)response_headers; *size = strlen(response_headers) + 1; return 0; }
static int32_t mock_read(int32_t req, void *target, size_t size)
{
    CHECK(resource[req].kind == REQUEST && resource[req].borrowed_url[0]);
    if (fail_read) { fail_read = false; return -78; }
    size_t total = strlen(response_body), offset = resource[req].offset;
    size_t take = total - offset < size ? total - offset : size;
    memcpy(target, response_body + offset, take);
    resource[req].offset += take;
    return (int32_t)take;
}
static int32_t mock_delete(int32_t id, enum ResourceKind kind)
{
    CHECK(resource[id].kind == kind);
    if ((kind == REQUEST && fail_delete_request) || (kind == CONNECTION && fail_delete_connection)) return -79;
    for (unsigned i = 1; i <= next_id; ++i) CHECK(resource[i].kind == NONE || resource[i].parent != id);
    if (kind != TEMPLATE) CHECK(resource[id].borrowed_url && resource[id].borrowed_url[0]);
    resource[id] = (struct Resource){0};
    return 0;
}
static int32_t mock_delete_template(int32_t id) { return mock_delete(id, TEMPLATE); }
static int32_t mock_delete_connection(int32_t id) { return mock_delete(id, CONNECTION); }
static int32_t mock_delete_request(int32_t id) { return mock_delete(id, REQUEST); }

int x4_module_open(const char *name)
{ CHECK(!strcmp(name, "libSceNet") || !strcmp(name, "libSceSsl") || !strcmp(name, "libSceHttp")); return 1; }
int x4_module_symbol(int handle, const char *name, void **address)
{
    CHECK(handle == 1);
#define SYMBOL(symbol, function) if (!strcmp(name, symbol)) { *address = (void *)(function); return 0; }
    SYMBOL("sceNetInit", mock_zero)
    SYMBOL("sceNetPoolCreate", mock_pool)
    SYMBOL("sceNetPoolDestroy", mock_pool_destroy)
    SYMBOL("sceSslInit", mock_ssl)
    SYMBOL("sceSslTerm", mock_ssl_term)
    SYMBOL("sceHttpInit", mock_http)
    SYMBOL("sceHttpTerm", mock_http_term)
    SYMBOL("sceHttpCreateTemplate", mock_template)
    SYMBOL("sceHttpCreateConnectionWithURL", mock_connection)
    SYMBOL("sceHttpCreateRequestWithURL", mock_request)
    SYMBOL("sceHttpDeleteTemplate", mock_delete_template)
    SYMBOL("sceHttpDeleteConnection", mock_delete_connection)
    SYMBOL("sceHttpDeleteRequest", mock_delete_request)
    SYMBOL("sceHttpSetResolveTimeOut", mock_timeout)
    SYMBOL("sceHttpSetConnectTimeOut", mock_timeout)
    SYMBOL("sceHttpSetSendTimeOut", mock_timeout)
    SYMBOL("sceHttpSetRecvTimeOut", mock_timeout)
    SYMBOL("sceHttpSetAutoRedirect", mock_redirect)
    SYMBOL("sceHttpSetCookieEnabled", mock_cookie)
    SYMBOL("sceHttpAddRequestHeader", mock_header)
    SYMBOL("sceHttpsEnableOption", mock_tls)
    SYMBOL("sceHttpSendRequest", mock_send)
    SYMBOL("sceHttpReadData", mock_read)
    SYMBOL("sceHttpGetStatusCode", mock_status)
    if (missing_header_api && !strcmp(name, "sceHttpGetAllResponseHeaders")) { *address = NULL; return -1; }
    SYMBOL("sceHttpGetAllResponseHeaders", mock_headers)
#undef SYMBOL
    CHECK(0); return -1;
}
static const char *store = "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details";
static const char *image = "https://store-images.s-microsoft.com/image/apps.test?w=512&q=80&format=jpg";
static X4Http *open_context(void)
{ int error = -1; X4Http *h = x4_http_open(&error); CHECK(h && !error); return h; }
static void public_exchange(X4Http *h, bool cover)
{
    char body[64]; size_t length = 99; int status = 99;
    int rc = cover ? x4_http_public_image_request(h, image, (unsigned char *)body, sizeof(body), &length, &status, NULL) :
        x4_http_public_metadata_request(h, store, body, sizeof(body), &length, &status, NULL);
    CHECK(!rc && length == strlen(response_body) && status == response_status && !strcmp(body, response_body));
}
static void test_reuse_isolation_cleanup(void)
{
    X4Http *h = open_context(); unsigned before = connections;
    for (unsigned i = 0; i < 6; ++i) { public_exchange(h, false); public_exchange(h, true); }
    CHECK(connections - before == 2); CHECK(h->public_connection[0].conn != h->public_connection[1].conn);
    CHECK(h->public_connection[0].anchor && h->public_connection[1].anchor);
    char body[64]; size_t length; int status;
    before = private_connections;
    CHECK(!x4_http_request(h, "https://login.microsoftonline.com/consumers/oauth2", "test=credential", body,
        sizeof(body), &length, &status, NULL));
    CHECK(private_connections == before + 1);
    CHECK(h->public_connection[0].conn >= 0 && h->public_connection[1].conn >= 0);
    CHECK(!x4_http_public_reset(h)); CHECK(!h->public_connection[0].anchor && !h->public_connection[1].anchor);
    CHECK(h->public_connection[0].conn == -1 && h->public_connection[1].conn == -1);
    public_exchange(h, false); x4_http_close(h);
}
static void test_retry_and_cancellation(void)
{
    X4Http *h = open_context(); public_exchange(h, false); unsigned before = sends;
    fail_send = true; public_exchange(h, false); CHECK(sends - before == 2);
    before = sends; fail_read = true; public_exchange(h, false); CHECK(sends - before == 2);
    CHECK(!x4_http_public_reset(h));
    char body[64]; size_t length = 0; int status = 0; fail_send = true; before = sends;
    CHECK(x4_http_public_metadata_request(h, store, body, sizeof(body), &length, &status, NULL) == -77);
    CHECK(sends - before == 1 && !body[0] && !length && !status);
    public_exchange(h, false);
    _Atomic int cancel = 0; cancel_during_send = &cancel; before = sends;
    CHECK(x4_http_public_metadata_request(h, store, body, sizeof(body), &length, &status, &cancel) == X4_HTTP_CANCELLED);
    CHECK(sends - before == 1 && !length && !status && !body[0]); CHECK(h->public_connection[0].conn == -1);
    cancel_during_send = NULL; public_exchange(h, false);
    response_body = "an oversized response"; before = sends;
    CHECK(x4_http_public_metadata_request(h, store, body, 4, &length, &status, NULL) == X4_HTTP_BOUND);
    CHECK(sends - before == 1 && !length && !status && !body[0]);
    response_body = "{}"; x4_http_close(h);
}
static void test_policy_and_owner(void)
{
    X4Http *h = open_context(); char body[64]; size_t length; int status; unsigned before = connections;
    const char *bad[] = {"http://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details",
        "https://displaycatalog.mp.microsoft.com:443/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details",
        "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details&token=x",
        "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1,,ID2&market=MX&languages=es-MX&fieldsTemplate=Details",
        "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=I1,I2,I3,I4,I5,I6,I7,I8,I9&market=MX&languages=es-MX&fieldsTemplate=Details",
        "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=../bad&market=MX&languages=es-MX&fieldsTemplate=Details",
        "https://region.gssv-play-prod.xboxlive.com/v2/titles", "https://login.live.com/oauth20_token.srf"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        CHECK(x4_http_public_metadata_request(h, bad[i], body, sizeof(body), &length, &status, NULL) == X4_HTTP_URL);
    CHECK(connections == before);
    const char *batch = "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=I1,I2,I3,I4,I5,I6,I7,I8&market=MX&languages=es-MX&fieldsTemplate=Details";
    CHECK(!x4_http_public_metadata_request(h, batch, body, sizeof(body), &length, &status, NULL));
    CHECK(x4_http_public_image_request(h, store, (unsigned char *)body, sizeof(body), &length, &status, NULL) == X4_HTTP_URL);
    atomic_store(&h->busy, true); int conn = h->public_connection[0].conn;
    CHECK(x4_http_public_reset(h) == X4_HTTP_BUSY);
    CHECK(x4_http_public_metadata_request(h, store, body, sizeof(body), &length, &status, NULL) == X4_HTTP_BUSY);
    CHECK(h->public_connection[0].conn == conn); atomic_store(&h->busy, false);
    response_status = 302; public_exchange(h, false); response_status = 200;
    x4_http_close(h);
}
static void test_retry_after(void)
{
    CHECK(retry_after_value("15", 2, 0) == 15000);
    CHECK(retry_after_value("-1", 2, 0) == 0); CHECK(retry_after_value("1.5", 3, 0) == 0);
    CHECK(retry_after_value("999999999999999999999", 21, 0) == UINT64_MAX);
    CHECK(retry_after_value("Sun, 06 Nov 1994 08:49:37 GMT", 29, 784111767) == 10000);
    CHECK(retry_after_value("Sun, 06 Nov 1994 08:49:37 GMT", 29, 784111778) == 0);
    CHECK(retry_after_value("Tue, 29 Feb 2000 00:00:00 GMT", 29, 951782390) == 10000);
    CHECK(retry_after_value("Tue, 29 Feb 2001 00:00:00 GMT", 29, 0) == 0);
    const char *delta_headers = "HTTP/1.1 429\r\nretry-after:\t5 \r\n\r\n";
    CHECK(retry_after_headers(delta_headers, strlen(delta_headers) + 1, 0) == 5000);
    const char *duplicate = "Retry-After: 5\r\nRetry-After: 6\r\n";
    CHECK(retry_after_headers(duplicate, strlen(duplicate), 0) == 0);
    CHECK(retry_after_headers("Retry-After: invalid\r\n", 22, 0) == 0);
    X4Http *h = open_context(); response_status = 429; response_headers = "HTTP/1.1 429\r\nRetry-After: 15\r\n\r\n";
    unsigned before = sends; public_exchange(h, false); CHECK(sends - before == 1);
    CHECK(x4_http_public_retry_after_ms(h) == 15000);
    response_status = 200; response_headers = "HTTP/1.1 200\r\n\r\n"; public_exchange(h, true);
    CHECK(!x4_http_public_retry_after_ms(h)); x4_http_close(h);
    missing_header_api = true; h = open_context(); public_exchange(h, false);
    CHECK(!x4_http_public_retry_after_ms(h)); x4_http_close(h); missing_header_api = false;
}
static void test_quarantine(void)
{
    X4Http *h = open_context(); public_exchange(h, false);
    PublicConnection saved = h->public_connection[0];
    fail_delete_connection = true; CHECK(x4_http_public_reset(h) == X4_HTTP_UNUSABLE);
    CHECK(h->quarantined && h->unusable && h->public_connection[0].conn == saved.conn &&
        h->public_connection[0].anchor == saved.anchor);
    unsigned before = terminated; x4_http_close(h); CHECK(terminated == before);
    /* Fixture-only reclamation: no native object exists after mock_delete,
     * so intentional process-lifetime quarantine need not leak under ASan. */
    fail_delete_connection = false; CHECK(!mock_delete_connection(saved.conn)); CHECK(!mock_delete_template(saved.tmpl));
    free_public_owned(&saved.anchor);
    h = open_context(); public_exchange(h, false); saved = h->public_connection[0];
    fail_delete_request = true; char body[64]; size_t length; int status;
    CHECK(x4_http_public_metadata_request(h, store, body, sizeof(body), &length, &status, NULL) == X4_HTTP_UNUSABLE);
    CHECK(h->quarantined && !length && !status && !body[0]);
    unsigned retained_request = next_id;
    Owned *retained_input = (Owned *)((char *)resource[retained_request].borrowed_url - offsetof(Owned, text));
    x4_http_close(h); fail_delete_request = false;
    CHECK(!mock_delete_request((int32_t)retained_request)); CHECK(!mock_delete_connection(saved.conn)); CHECK(!mock_delete_template(saved.tmpl));
    free_public_owned(&retained_input); free_public_owned(&saved.anchor);
}
int main(void)
{
    test_reuse_isolation_cleanup(); test_retry_and_cancellation(); test_policy_and_owner(); test_retry_after(); test_quarantine();
    printf("production public HTTP fixture PASS: %u checks; %u public connections, %u requests\n", checks, public_connections, requests);
    return 0;
}
