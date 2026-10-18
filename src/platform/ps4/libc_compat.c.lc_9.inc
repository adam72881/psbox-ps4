));
}

int __wrap_pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abs) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (s && s->rec && s->depth > 0 && pthread_equal(s->owner, pthread_self())) {
        s->depth++;
        return 0;
    }
    pthread_mutex_t* raw = s ? (pthread_mutex_t*)s->raw : mutex;
    int spins = 0, rc;
    do {
        rc = xc_fold_sce(__real_pthread_mutex_timedlock(raw, abs));
    } while (rc == EINTR && ++spins < 8);
    if (rc == EINTR || xc_is_timeout(rc)) rc = XC_LIBCXX_ETIMEDOUT;
    if (rc != 0) return rc;
    if (s && s->rec) {
        s->owner = pthread_self();
        s->depth = 1;
    }
    return 0;
}

int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    static int n;
    int logged = 0;
    (void)attr;
    if (!cond) return EINVAL;
    if (xc_log_is_ready() && n < 16) {
        char msg[24];
        snprintf(msg, sizeof msg, "cond %d", ++n);
        xc_log_flush_line("I", msg);
        logged = 1;
    }
    struct xc_slot* s = slot_take(g_cond, XC_COND_MAX, cond);
    if (!s) {
        if (xc_log_is_ready()) xc_log_flush_line("W", "cond pieno");
        return ENOMEM;
    }
    memset(cond, 0, sizeof *cond);
    int rc = xc_fold_sce(__real_pthread_cond_init((pthread_cond_t*)s->raw, NULL));
    if (logged) {
        char msg[24];
        snprintf(msg, sizeof msg, "cond rc %d", rc);
        xc_log_flush_line("I", msg);
    }
    if (rc != 0) slot_drop(g_cond, XC_COND_MAX, cond);
    return rc;
}

int __wrap_pthread_cond_destroy(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    struct xc_slot* s = slot_find(g_cond, XC_COND_MAX, cond);
    if (!s) return __real_pthread_cond_destroy(cond);
    int rc = __real_pthread_cond_destroy((pthread_cond_t*)s->raw);
    slot_drop(g_cond, XC_COND_MAX, cond);
    return rc;
}

int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    return xc_cv_wait(__real_pthread_cond_wai