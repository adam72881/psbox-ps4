i].key, &expected, key, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            tab[i].rec = 0;
            tab[i].depth = 0;
            tab[i].owner = 0;
            memset(tab[i].raw, 0, sizeof tab[i].raw);
            return &tab[i];
        }
    }
    return NULL;
}

static void slot_drop(struct xc_slot* tab, int n, void* key) {
    struct xc_slot* s = slot_find(tab, n, key);
    if (!s) return;
    s->rec = 0;
    s->depth = 0;
    s->owner = 0;
    __atomic_store_n(&s->key, (void*)0, __ATOMIC_RELEASE);
}

static pthread_mutex_t* mtx_sony(pthread_mutex_t* mutex) {
    struct xc_slot* s = mutex ? slot_find(g_mtx, XC_MTX_MAX, mutex) : NULL;
    return s ? (pthread_mutex_t*)s->raw : mutex;
}

static pthread_cond_t* cond_sony(pthread_cond_t* cond) {
    struct xc_slot* s = cond ? slot_find(g_cond, XC_COND_MAX, cond) : NULL;
    return s ? (pthread_cond_t*)s->raw : cond;
}

int __wrap_pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    if (attr) memset(attr, 0, sizeof *attr);
    return 0;
}

int __wrap_pthread_mutexattr_destroy(pthread_mutexattr_t* attr) {
    (void)attr;
    return 0;
}

int __wrap_pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
    if (!attr) return EINVAL;
    attr->__attr = (unsigned)type;
    return 0;
}

int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    static int n;
    unsigned type = attr ? attr->__attr : 0;
    int rec = type == (unsigned)PTHREAD_MUTEX_RECURSIVE;
    int logged = 0;
    if (!mutex) return EINVAL;
    if (xc_log_is_ready() && n < 40) {
        char msg[32];
        snprintf(msg, sizeof msg, "mtx %s %d", rec ? "rec" : "std", ++n);
        xc_log_flush_line("I", msg);
        logged = 1;
    }
    struct xc_slot* s = slot_take(g_mtx, XC_MTX_MAX, mutex);
    if (!s) {
        if (xc_log_is_ready()) xc_log_flush_line("W", "mtx pieno");
        return ENOMEM;
    }
    s->rec = rec;
    memset(mutex, 0, sizeof *mutex);
    int rc = xc_fold_sce(__real_pthread_mutex_