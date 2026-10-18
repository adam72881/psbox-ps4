  xc_log_flush_line("I", "rand");
    xc_log_flush_line("I", "drbg");
    int rc = __real_mbedtls_ctr_drbg_seed(ctx, f_entropy, p_entropy, custom, len);
    char msg[24];
    snprintf(msg, sizeof msg, "drbg %d", rc);
    xc_log_flush_line("I", msg);
    return rc;
}

/* usrsctp, compilato come FreeBSD, apre AF_ROUTE e SOCK_RAW. Su Orbis quel
 * socket non esiste. getifaddrs di musl usa netlink (AF_NETLINK + SOCK_RAW):
 * lo stesso buco. I dati SCTP di WebRTC passano dal callback, non dal kernel. */
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
int __wrap_socket(int domain, int type, int protocol) {
    int kind = type & 0xf;
    /* Orbis non ha il dual-stack: IPV6_V6ONLY risponde e42 e sendto fam 28
     * torna EINVAL. libjuice deve aprire solo AF_INET. */
    if (domain == AF_INET6) {
        static int once;
        if (!once++) xc_log_flush_line("W", "socket: niente ipv6, solo AF_INET");
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (domain == AF_ROUTE || domain == AF_UNSPEC || domain == AF_NETLINK || kind == SOCK_RAW) {
        char msg[40];
        snprintf(msg, sizeof msg, "socket no %d/%d/%d", domain, type, protocol);
        xc_log_flush_line("W", msg);
        errno = EPROTONOSUPPORT;
        return -1;
    }
    int fd = __real_socket(domain, type, protocol);
    if (fd < 0) xc_fold_errno();
    /* RCVBUF Orbis: lo imposta juice (PSBOX_RCVBUF) e lo remap __wrap_setsockopt
     * (8→0x1002). Niente drain: sulla 01.25 rubava i pacchetti a juice. */
    static int n;
    if (xc_log_is_ready() && n < 12) {
        char msg[48];
        snprintf(msg, sizeof msg, "socket %d/%d/%d => %d", domain, kind, protocol, fd);
        xc_log_flush_line("I", msg);
        ++n;
    }
    return fd;
}

int __wrap_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    uintptr_t arg = va_arg(ap, uintptr_t);
    va_end(ap);
    int rc = __real_fcntl(fd, cmd, arg);
    /* La sandbox rifiuta O_NONBLOCK sui socket (come sulla PS5). SO_NBIO sì.
 