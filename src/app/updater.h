// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Updates the app in place from a release (update_check.h).
//
// The release's PPSA99810.zip must carry a signature (PPSA99810.zip.sig,
// ECDSA P-256 over SHA-256) by the project's own key, which tools/release.sh
// makes and only the maintainer holds: kReleaseKey here is its public half.
// Nothing is changed without it, nor for a version that isn't newer.
//
// The files are unpacked into <dir>/update.new, then each one in place is
// moved to <dir>/update.old and the new one put there (eboot.bin last); the
// running app keeps its old eboot.bin until it restarts. <dir>/update.journal
// is there while files move: a start that finds it puts the old ones back.
// The user's files (sign-in, settings, caches, log) are never touched.
#pragma once

#include "app/update_check.h"

#include <functional>
#include <string>
#include <vector>

namespace xc::app {

// The project's release signing key (public).
extern the_char_placeholder;
