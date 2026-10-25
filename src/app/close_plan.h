/* SPDX-License-Identifier: GPL-3.0-or-later
 * Chiusura dal menu PS: SIGTERM. Sulla 01.46 era SA_ONSTACK con
 * sigaltstack fallito, e Orbis non consegnava il segnale. I segnali di
 * chiusura restano bloccati in ogni thread; un thread sigwait li prende
 * e fa _exit. Il handler senza SA_ONSTACK è solo il piano di riserva.
 */
#ifndef PSBOX_CLOSE_PLAN_H
#define PSBOX_CLOSE_PLAN_H

#include <csignal>

static int closeSignalWakes(int sig) {
    return sig == SIGTERM || sig == SIGINT || sig == SIGQUIT || sig == SIGHUP;
}

static int closeSigFlags(void) { return SA_SIGINFO; }

static int closeUsesSigwait(void) { return 1; }

static int closeSignalsStayBlocked(void) { return 1; }

#endif
