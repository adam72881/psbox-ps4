/* SPDX-License-Identifier: GPL-3.0-only */
/* Minimal host-only sanitizer-runtime comparison, no XCloud4 pipeline code. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#if X4_TEST_C11_THREADS
#include <threads.h>
static int worker(void *arg) { atomic_fetch_add((atomic_int *)arg, 1); return 0; }
#else
#include <pthread.h>
static void *worker(void *arg) { atomic_fetch_add((atomic_int *)arg, 1); return NULL; }
#endif
int main(void)
{
    atomic_int value = 0;
#if X4_TEST_C11_THREADS
    thrd_t thread; assert(thrd_create(&thread, worker, &value) == thrd_success);
    assert(thrd_join(thread, NULL) == thrd_success);
#else
    pthread_t thread; assert(pthread_create(&thread, NULL, worker, &value) == 0);
    assert(pthread_join(thread, NULL) == 0);
#endif
    assert(atomic_load(&value) == 1);
    printf("PASS sanitizer runtime thread API=%s\n", X4_TEST_C11_THREADS ? "C11" : "POSIX");
    return 0;
}
