00000L;
            }
            ts = &rt;
        }
    }
    return __wrap_pthread_cond_timedwait(cond, mutex, ts);
}

int __wrap_pthread_cond_signal(pthread_cond_t* cond) {
    return xc_fold_sce(__real_pthread_cond_signal(cond_sony(cond)));
}

int __wrap_pthread_cond_broadcast(pthread_cond_t* cond) {
    return xc_fold_sce(__real_pthread_cond_broadcast(cond_sony(cond)));
}

int __wrap_scePthreadCondTimedwait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx, OrbisKernelUseconds usec) {
    return xc_cv_timed(__real_scePthreadCondTimedwait(cond, mtx, usec));
}

int __wrap_scePthreadCondWait(OrbisPthreadCond* cond, OrbisPthreadMutex* mtx) {
    return xc_cv_wait(__real_scePthreadCondWait(cond, mtx));
}

void __real_usrsctp_init(uint16_t port, int (*output)(void*, void*, size_t, uint8_t, uint8_t),
                         void (*debug)(const char*, ...));

void __wrap_usrsctp_init(uint16_t port, int (*output)(void*, void*, size_t, uint8_t, uint8_t),
                         void (*debug)(const char*, ...)) {
    xc_log_flush_line("I", "usrsctp");
    __real_usrsctp_init(port, output, debug);
    xc_log_flush_line("I", "usrsctp ok");
}

void* __real_juice_create(const void* config);
void* __wrap_juice_create(const void* config) {
    xc_log_flush_line("I", "juice");
    void* agent = __real_juice_create(config);
    xc_log_flush_line("I", agent ? "juice ok" : "juice no");
    return agent;
}

void __real_mbedtls_entropy_init(void* ctx);
void __wrap_mbedtls_entropy_init(void* ctx) {
    xc_log_flush_line("I", "entropy");
    __real_mbedtls_entropy_init(ctx);
    xc_log_flush_line("I", "entropy ok");
}

int __real_mbedtls_ctr_drbg_seed(void* ctx, int (*f_entropy)(void*, unsigned char*, size_t), void* p_entropy,
                                 const unsigned char* custom, size_t len);
int __wrap_mbedtls_ctr_drbg_seed(void* ctx, int (*f_entropy)(void*, unsigned char*, size_t), void* p_entropy,
                                 const unsigned char* custom, size_t len) {
  