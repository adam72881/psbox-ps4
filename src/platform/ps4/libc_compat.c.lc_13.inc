    * Sulle pipe setsockopt fallisce e resta il fcntl reale. */
    if (cmd == F_SETFL && ((int)arg & O_NONBLOCK)) {
        int on = 1;
        if (setsockopt(fd, SOL_SOCKET, ORBIS_SO_NBIO, &on, sizeof on) == 0) return 0;
    }
    return rc;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (request == FIONBIO && arg) {
        int on = *(const int*)arg ? 1 : 0;
        if (setsockopt(fd, SOL_SOCKET, ORBIS_SO_NBIO, &on, sizeof on) == 0) return 0;
    }
    /* SIOCGIF* di Linux (0x89xx) non è un ioctl di Orbis: passarlo al kernel
     * è il passo che fa morire getifaddrs/MTU. MTU finto 1500, il resto no. */
    if (request == SIOCGIFMTU && arg) {
        struct ifreq* ifr = arg;
        ifr->ifr_mtu = 1500;
        static int once;
        if (!once++) xc_log_flush_line("I", "ioctl mtu 1500");
        return 0;
    }
    if (request == SIOCGIFCONF || request == SIOCGIFADDR || request == SIOCGIFFLAGS ||
        request == SIOCGIFNETMASK || request == SIOCGIFINDEX) {
        static int n;
        if (n < 4) {
            char msg[32];
            snprintf(msg, sizeof msg, "ioctl if %lx", request);
            xc_log_flush_line("W", msg);
            ++n;
        }
        errno = ENOTTY;
        return -1;
    }
    return __real_ioctl(fd, request, arg);
}

static struct addrinfo* make_ai(int family, const void* addr, unsigned scope, int port, const struct addrinfo* hints) {
    size_t salen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    struct addrinfo* ai = calloc(1, sizeof *ai + salen);
    if (!ai) return NULL;
    struct sockaddr* sa = (struct sockaddr*)(ai + 1);
    if (family == AF_INET) {
        struct sockaddr_in* s4 = (struct sockaddr_in*)sa;
        s4->sin_len = sizeof *s4;
        s4->sin_family = AF_INET;
        s4->sin_port = htons((uint16_t)port);
        memcpy(&s4->sin_addr, addr, 4);
    } else {
     