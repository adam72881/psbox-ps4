dCreate non è tornato oppure lo stack non è mappato. */
struct xc_job {
    void* (*fn)(void*);
    void* arg;
};

static void* xc_enter(void* raw) {
    struct xc_job* job = raw;
    void* (*fn)(void*) = job->fn;
    void* arg = job->arg;
    free(job);
    unsigned long tp = 0;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
    char msg[48];
    snprintf(msg, sizeof msg, "in %p fs %lx", (void*)fn, tp);
    xc_log_raw(msg);
    return fn(arg);
}

static int xc_spawn(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    struct xc_job* job = malloc(sizeof *job);
    if (!job) return __real_pthread_create(thread, attr, start, arg);
    job->fn = start;
    job->arg = arg;
    char msg[40];
    snprintf(msg, sizeof msg, "go %p", (void*)start);
    xc_log_flush_line("I", msg);
    int rc = xc_fold_sce(__real_pthread_create(thread, attr, xc_enter, job));
    snprintf(msg, sizeof msg, "rc %d", rc);
    xc_log_flush_line("I", msg);
    if (rc != 0) free(job);
    return rc;
}

/* Chi chiede già almeno 2 MiB (video, audio, worker) lo tiene. Gli altri,
 * compreso il pool di libdatachannel che passa attr NULL, restano a 2 MiB:
 * quattro stack da 8 MiB dopo i framebuffer di Videodec2 fanno morire
 * scePthreadCreate senza un codice di errore (CE-34878-0). 2 MiB bastano
 * per DTLS. */
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    const size_t kFloor = 2u << 20;
    size_t sz = 0;
    if (attr && pthread_attr_getstacksize(attr, &sz) == 0 && sz >= kFloor) {
        log_stack(sz, sz);
        return xc_spawn(thread, attr, start, arg);
    }

    pthread_attr_t big;
    if (pthread_attr_init(&big) != 0) return xc_spawn(thread, attr, start, arg);
    int detach = PTHREAD_CREATE_JOINABLE;
    if (attr) pthread_attr_getdetachstate(attr, &detach);
    pthread_attr_setdetachstate(&big, detach);
    if (pthread_attr_setstacksize(&big, kFloor) != 0) {
        pthread_attr_destroy(&b