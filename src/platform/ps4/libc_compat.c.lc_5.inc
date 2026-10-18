int __real_nanosleep(const struct timespec* req, struct timespec* rem);
ssize_t __real_read(int fd, void* buf, size_t n);
ssize_t __real_write(int fd, const void* buf, size_t n);
ssize_t __real_recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* addr, socklen_t* len);
ssize_t __real_sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len);
int __real_connect(int fd, const struct sockaddr* addr, socklen_t len);
int __real_poll(struct pollfd* fds, nfds_t n, int timeout);
int32_t __real_scePthreadCondTimedwait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx, OrbisKernelUseconds usec);
int32_t __real_scePthreadCondWait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx);

/* Orbis risponde 0x80020000 | errno di FreeBSD (ETIMEDOUT = 60, quindi
 * 0x8002003C). libc++ di questo eboot, in condition_variable::__do_timed_wait,
 * confronta il ritorno con 0x6e (110, l'ETIMEDOUT di Linux): 60 != 110 e
 * la funzione è noexcept, quindi terminate chiama abort. I socket invece
 * restano sull'errno di musl: mbedtls confronta EAGAIN (35). */
enum { XC_LIBCXX_ETIMEDOUT = 110 };

static int xc_fold_sce(int rc) {
    unsigned u = (unsigned)rc;
    if ((u & 0xFFFF0000u) == 0x80020000u) {
        int e = (int)(u & 0xFFFFu);
        return e == 0 ? EINVAL : e;
    }
    return rc;
}

static int xc_is_timeout(int e) { return e == 60 || e == XC_LIBCXX_ETIMEDOUT || e == 9938; }

/* Ritorno di pthread_cond_timedwait: 0 resta 0, il timeout diventa 110,
 * EINTR è un risveglio spurio (altrimenti libc++ abortisce). */
static int xc_cv_timed(int rc) {
    unsigned raw = (unsigned)rc;
    int e = xc_fold_sce(rc);
    if (e == 0) return 0;
    if (xc_is_timeout(e)) {
        static int once;
        if (!once++) {
            char msg[48];
            snprintf(msg, sizeof msg, "cond timeout 0x%x -> 110", raw);
            xc_log_stamp(msg);
        }
        return XC_LIBCXX_ETIMEDOUT;
    }
    if (e == EINTR) return 0;
    static int n;
    if (n 