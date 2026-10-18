init((pthread_mutex_t*)s->raw, NULL));
    if (logged) {
        char msg[24];
        snprintf(msg, sizeof msg, "mtx rc %d", rc);
        xc_log_flush_line("I", msg);
    }
    if (rc != 0) slot_drop(g_mtx, XC_MTX_MAX, mutex);
    return rc;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    struct xc_slot* s = slot_find(g_mtx, XC_MTX_MAX, mutex);
    if (!s) return __real_pthread_mutex_destroy(mutex);
    int rc = xc_fold_sce(__real_pthread_mutex_destroy((pthread_mutex_t*)s->raw));
    slot_drop(g_mtx, XC_MTX_MAX, mutex);
    return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (!s) return xc_lock_raw(mutex);
    if (s->rec && s->depth > 0 && pthread_equal(s->owner, pthread_self())) {
        s->depth++;
        return 0;
    }
    int rc = xc_lock_raw((pthread_mutex_t*)s->raw);
    if (rc != 0) return rc;
    if (s->rec) {
        s->owner = pthread_self();
        s->depth = 1;
    }
    return 0;
}

int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (!s) return xc_fold_sce(__real_pthread_mutex_trylock(mutex));
    if (s->rec && s->depth > 0 && pthread_equal(s->owner, pthread_self())) {
        s->depth++;
        return 0;
    }
    int rc = xc_fold_sce(__real_pthread_mutex_trylock((pthread_mutex_t*)s->raw));
    if (rc != 0) return rc;
    if (s->rec) {
        s->owner = pthread_self();
        s->depth = 1;
    }
    return 0;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    if (!s) return __real_pthread_mutex_unlock(mutex);
    if (s->rec && s->depth > 1 && pthread_equal(s->owner, pthread_self())) {
        s->depth--;
        return 0;
    }
    s->depth = 0;
    return xc_fold_sce(__real_pthread_mutex_unlock((pthread_mutex_t*)s->raw