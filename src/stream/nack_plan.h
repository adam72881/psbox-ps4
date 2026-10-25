/* SPDX-License-Identifier: GPL-3.0-or-later
 * Quanto si aspetta un buco prima di mollare il frame e chiedere un IDR.
 * 150 ms teneva ferma l'immagine; 45 ms è un paio di RTT, poi si risincronizza.
 */
#ifndef PSBOX_NACK_PLAN_H
#define PSBOX_NACK_PLAN_H

#include <stdint.h>

static const uint64_t kNackGraceMs = 45;
static const uint64_t kNackRepeatMs = 20;

#endif
