   struct sockaddr_in6* s6 = (struct sockaddr_in6*)sa;
        s6->sin6_len = sizeof *s6;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((uint16_t)port);
        s6->sin6_scope_id = scope;
        memcpy(&s6->sin6_addr, addr, 16);
    }
    ai->ai_family = family;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_flags = hints ? hints->ai_flags : 0;
    ai->ai_addrlen = (socklen_t)salen;
    ai->ai_addr = sa;
    return ai;
}

int __wrap_getaddrinfo(const char* host, const char* serv, const struct addrinfo* hints, struct addrinfo** res) {
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int flags = hints ? hints->ai_flags : 0;
    int port = 0;
    if (!res) return EAI_FAIL;
    *res = NULL;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return EAI_FAMILY;
    if (serv && *serv) {
        char* end = NULL;
        long p = strtol(serv, &end, 10);
        if (*end || p < 0 || p > 65535) {
            if (!strcmp(serv, "https")) p = 443;
            else if (!strcmp(serv, "http")) p = 80;
            else return EAI_SERVICE;
        }
        port = (int)p;
    }

    unsigned char a[16];
    if (family == AF_INET6) return EAI_FAMILY;
    if (!host) {
        /* Solo IPv4: un risultato AF_INET6 fa aprire a juice un socket che
         * poi manda ogni STUN come fam 28. */
        uint32_t v4 = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
        return (*res = make_ai(AF_INET, &v4, 0, port, hints)) ? 0 : EAI_MEMORY;
    }
    if (inet_pton(AF_INET, host, a) == 1)
        return (*res = make_ai(AF_INET, a, 0, port, hints)) ? 0 : EAI_MEMORY;
    if (flags & AI_NUMERICHOST) return EAI_NONAME;
    if (family == AF_INET6) return EAI_NONAME;
    uint32_t v4 = 0;
    if (xc_resolve_ipv4(host, &v4) != 0) return EAI_NONAME;
    return (*res = make_ai(AF_INET, &v4, 0, port, hints)) ? 0 : EAI_MEMORY;
}

void __wrap_freeaddrinfo(struct addrinfo