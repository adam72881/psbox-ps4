/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only shapes and button constants, not a native ABI claim. */
#pragma once
#include <stdint.h>
enum { ORBIS_PAD_BUTTON_OPTIONS=1, ORBIS_PAD_BUTTON_CIRCLE=2 };
typedef struct { uint32_t buttons; } OrbisPadData;
