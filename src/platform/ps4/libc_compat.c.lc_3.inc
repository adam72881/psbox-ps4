ig);
        xc_log_flush_line("W", "pthread: setstacksize fallito");
        return xc_spawn(thread, attr, start, arg);
    }
    log_stack(kFloor, sz);
    int rc = xc_spawn(thread, &big, start, arg);
    pthread_attr_destroy(&big);
    return rc;
}

/* libc non esporta pthread_set_name_np: il primo thread di libdatachannel
 * ci salta dentro e il processo muore con CE-34878-0. Il nome non serve. */
int pthread_set_name_np(pthread_t thread, const char* name) {
    (void)thread;
    (void)name;
    return 0;
}

/* Il pthread_rwlock di Sony non è quello di musl: l'oggetto Orbis è un
 * puntatore (inizializzatore NULL) e la seconda pthread_rwlock_init di
 * usrsctp, su Fortnite con la 01.02, uccide il processo dopo "rwinit 2".
 * Un mutex nello spazio del rwlock basta: SCTP non ha lettori in parallelo.
 * Le operazioni sotto sono tutte quelle che libusrsctp chiama. */
_Static_assert(sizeof(pthread_mutex_t) <= sizeof(pthread_rwlock_t), "mutex nel rwlock");

static pthread_mutex_t* rw_as_mutex(pthread_rwlock_t* lock) { return (pthread_mutex_t*)lock; }

int __wrap_pthread_rwlockattr_init(pthread_rwlockattr_t* attr) {
    if (attr) memset(attr, 0, sizeof *attr);
    xc_log_flush_line("I", "rwlock 0");
    return 0;
}

int __wrap_pthread_rwlockattr_destroy(pthread_rwlockattr_t* attr) {
    (void)attr;
    return 0;
}

int __wrap_pthread_rwlock_init(pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr) {
    (void)attr;
    static int n;
    int i = ++n;
    int rc = EINVAL;
    if (lock) {
        memset(lock, 0, sizeof *lock);
        rc = pthread_mutex_init(rw_as_mutex(lock), NULL);
    }
    if (i <= 4) {
        char msg[32];
        snprintf(msg, sizeof msg, "rwinit %d rc %d", i, rc);
        xc_log_flush_line("I", msg);
    }
    return rc;
}

int __wrap_pthread_rwlock_destroy(pthread_rwlock_t* lock) {
    return lock ? pthread_mutex_destroy(rw_as_mutex(lock)) : EINVAL;
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* lock) { return pthread_mutex_lock(rw_as