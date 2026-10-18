* ai) {
    while (ai) {
        struct addrinfo* next = ai->ai_next;
        free(ai);
        ai = next;
    }
}

int __wrap_getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, size_t hostlen, char* serv,
                       size_t servlen, int flags) {
    (void)salen;
    (void)flags;
    int port = 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* s4 = (const struct sockaddr_in*)sa;
        if (host && hostlen && !inet_ntop(AF_INET, &s4->sin_addr, host, (socklen_t)hostlen)) return EAI_OVERFLOW;
        port = ntohs(s4->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* s6 = (const struct sockaddr_in6*)sa;
        if (host && hostlen && !inet_ntop(AF_INET6, &s6->sin6_addr, host, (socklen_t)hostlen)) return EAI_OVERFLOW;
        port = ntohs(s6->sin6_port);
    } else {
        return EAI_FAMILY;
    }
    if (serv && servlen) snprintf(serv, servlen, "%d", port);
    return 0;
}

/* usrsctp, sul percorso FreeBSD, chiama arc4random_buf. La libc musl non lo ha. */
int ps4_random_bytes(void* out, size_t len);

void arc4random_buf(void* buf, size_t n) {
    if (!buf || n == 0) return;
    if (!ps4_random_bytes(buf, n)) memset(buf, 0, n);
}

/* libc++ chiama __cxa_thread_atexit_impl, che musl di OpenOrbis non esporta.
 * I distruttori thread_local girano alla fine del thread, come su FreeBSD. */
struct xc_tls_dtor {
    void (*dtor)(void*);
    void* obj;
    struct xc_tls_dtor* next;
};

static pthread_key_t xc_tls_key;
static pthread_once_t xc_tls_once = PTHREAD_ONCE_INIT;

static void xc_run_tls_dtors(void* p) {
    struct xc_tls_dtor* n = p;
    while (n) {
        struct xc_tls_dtor* next = n->next;
        n->dtor(n->obj);
        free(n);
        n = next;
    }
}

static void xc_tls_key_init(void) { pthread_key_create(&xc_tls_key, xc_run_tls_dtors); }

int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* dso) {
    struct xc_tls_dtor* n;
    (void)dso;
    if (!dtor)