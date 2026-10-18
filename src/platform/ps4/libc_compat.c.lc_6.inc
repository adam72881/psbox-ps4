< 4) {
        char msg[48];
        snprintf(msg, sizeof msg, "cond rc 0x%x -> %d", raw, e);
        xc_log_stamp(msg);
        ++n;
    }
    return e;
}

static int xc_cv_wait(int rc) {
    int e = xc_fold_sce(rc);
    if (e == 0 || e == EINTR || xc_is_timeout(e)) return 0;
    return e;
}

static int xc_lock_raw(pthread_mutex_t* raw) {
    int spins = 0, rc;
    do {
        rc = xc_fold_sce(__real_pthread_mutex_lock(raw));
    } while (rc == EINTR && ++spins < 16);
    return rc;
}

static void xc_fold_errno(void) { errno = xc_fold_sce(errno); }

static int xc_sys_int(int rc) {
    if (rc == 0) return 0;
    if (rc < 0) {
        xc_fold_errno();
        return -1;
    }
    unsigned u = (unsigned)rc;
    if ((u & 0xFFFF0000u) == 0x80020000u) {
        errno = xc_fold_sce(rc);
        return -1;
    }
    return rc;
}

static ssize_t xc_sys_sz(ssize_t rc) {
    if (rc >= 0) return rc;
    xc_fold_errno();
    return -1;
}

#define XC_SONY_OBJ 256
/* Il PeerConnection, usrsctp, libjuice e mbedtls insieme superano 128 mutex.
 * Ogni slot è 256 byte: 512 mutex e 128 cond restano sotto i 200 KB. */
#define XC_MTX_MAX 512
#define XC_COND_MAX 128

struct xc_slot {
    void* key;
    int rec;
    int depth;
    pthread_t owner;
    _Alignas(16) unsigned char raw[XC_SONY_OBJ];
};

static struct xc_slot g_mtx[XC_MTX_MAX];
static struct xc_slot g_cond[XC_COND_MAX];

static struct xc_slot* slot_find(struct xc_slot* tab, int n, void* key) {
    for (int i = 0; i < n; i++) {
        if (__atomic_load_n(&tab[i].key, __ATOMIC_ACQUIRE) == key) return &tab[i];
    }
    return NULL;
}

static struct xc_slot* slot_take(struct xc_slot* tab, int n, void* key) {
    struct xc_slot* have = slot_find(tab, n, key);
    if (have) {
        have->rec = 0;
        have->depth = 0;
        have->owner = 0;
        memset(have->raw, 0, sizeof have->raw);
        return have;
    }
    for (int i = 0; i < n; i++) {
        void* expected = NULL;
        if (__atomic_compare_exchange_n(&tab[