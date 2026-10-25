/* SPDX-License-Identifier: GPL-3.0-only
 * PSBox overlay: chiusura dal menu PS (SIGTERM) → _exit(0) sullo stack
 * normale, senza dtor libdatachannel/Videodec2 (CE-34878-0 / EXIT ERROR
 * post-ICE in PSBox 01.55–01.56). Portato dal crash_guard PSBox.
 */
#include "psbox_exit.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void close_set(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGQUIT);
    sigaddset(set, SIGHUP);
}

static void *sigwait_loop(void *arg)
{
    (void)arg;
    sigset_t set;
    close_set(&set);
    for (;;) {
        int sig = 0;
        if (sigwait(&set, &sig) != 0) continue;
        if (sig != SIGTERM && sig != SIGINT && sig != SIGQUIT && sig != SIGHUP) continue;
        const char msg[] = "exit: processo terminato (segnale)\n";
        (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
        _exit(0);
    }
    return NULL;
}

static void on_term(int sig)
{
    (void)sig;
    const char msg[] = "exit: processo terminato (handler)\n";
    (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
    _exit(0);
}

int psbox_exit_install(void)
{
    sigset_t set;
    close_set(&set);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) return -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* stack normale: SA_ONSTACK su Orbis non consegna TERM */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_t th;
    int rc = pthread_create(&th, &attr, sigwait_loop, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) return -1;
    pthread_detach(th);
    printf("PSBox: SIGTERM/INT/QUIT/HUP bloccati, sigwait → _exit(0)\n");
    return 0;
}
