t(cond_sony(cond), mtx_sony(mutex)));
}

int __wrap_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    return xc_cv_timed(__real_pthread_cond_timedwait(cond_sony(cond), mtx_sony(mutex), abstime));
}

/* musl numera gli orologi come Linux (MONOTONIC = 1). Il kernel Orbis è
 * FreeBSD: 1 è CLOCK_VIRTUAL, il tempo CPU del processo, che avanza circa
 * 23 volte più piano del muro. Il monotonic vero è 4. Un'attesa di 1 s
 * sul clock 1 tornava dopo ~23 s di orologio da parete: il ClientHello
 * veniva ritrasmesso quando il server aveva già chiuso il DTLS. */
static clockid_t xc_orbis_clock(clockid_t clk) {
    switch ((int)clk) {
    case 0: return 0; /* REALTIME */
    case 1: return 4; /* linux MONOTONIC -> freebsd MONOTONIC */
    case 2: return 2;
    case 3: return 3;
    case 4: return 4; /* MONOTONIC_RAW linux, e il monotonic freebsd */
    case 5: return 0; /* REALTIME_COARSE */
    case 6: return 4; /* MONOTONIC_COARSE */
    case 7: return 4; /* BOOTTIME */
    default: return (clockid_t)4;
    }
}

/* libc++ con CLOCK_MONOTONIC chiama questa, che su Orbis non c'è.
 * Il tempo assoluto monotonic si traduce in tempo reale e si usa timedwait. */
int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t clk, const struct timespec* abstime) {
    struct timespec rt;
    const struct timespec* ts = abstime;
    if (abstime && clk != CLOCK_REALTIME) {
        struct timespec now, real;
        if (clock_gettime(clk, &now) == 0 && clock_gettime(CLOCK_REALTIME, &real) == 0) {
            long sec = (long)abstime->tv_sec - (long)now.tv_sec;
            long nsec = abstime->tv_nsec - now.tv_nsec;
            if (nsec < 0) {
                sec--;
                nsec += 1000000000L;
            }
            rt.tv_sec = real.tv_sec + sec;
            rt.tv_nsec = real.tv_nsec + nsec;
            if (rt.tv_nsec >= 1000000000L) {
                rt.tv_sec++;
                rt.tv_nsec -= 10000