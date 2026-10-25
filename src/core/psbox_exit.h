/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Installa sigwait+_exit prima di qualsiasi altro thread. */
int psbox_exit_install(void);
