_mutex(lock)); }

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* lock) { return pthread_mutex_lock(rw_as_mutex(lock)); }

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t* lock) { return pthread_mutex_unlock(rw_as_mutex(lock)); }

int __wrap_pthread_rwlock_tryrdlock(pthread_rwlock_t* lock) { return pthread_mutex_trylock(rw_as_mutex(lock)); }

/* L'attributo mutex di Sony è un puntatore da 8 byte; quello di musl ne ha 4.
 * Passarlo a pthread_mutex_init uccide il processo. A Sony arriva sempre NULL.
 * Il tipo ricorsivo è un mutex normale più un contatore nello slot.
 *
 * pthread_mutex_t di musl è 40 byte. scePthreadMutexInit scrive l'oggetto
 * Orbis, più grande: sulla 01.06 cinque std::recursive_mutex tornavano
 * "mtx rc 0" e il sesto passo (il callback dopo) moriva, prima di "pc body".
 * Lo scritto di Sony finiva nel membro C++ successivo. L'init va in un buffer
 * da 256 byte; lock/unlock usano quello. I mutex con PTHREAD_MUTEX_INITIALIZER
 * non passano di qui e restano sull'oggetto musl, come prima. */
int xc_log_is_ready(void);
int __real_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int __real_pthread_mutex_destroy(pthread_mutex_t* mutex);
int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t* mutex);
int __real_pthread_mutex_trylock(pthread_mutex_t* mutex);
int __real_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int __real_pthread_cond_destroy(pthread_cond_t* cond);
int __real_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int __real_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime);
int __real_pthread_cond_signal(pthread_cond_t* cond);
int __real_pthread_cond_broadcast(pthread_cond_t* cond);
int __real_pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abs);
int __real_sem_timedwait(sem_t* sem, const struct timespec* abs);
int __real_sem_wait(sem_t* sem);
