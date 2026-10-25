// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace xc::app {

// /data/psbox/autostart.txt: una riga, nome del gioco oppure title id xCloud
// (il payload ci scrive "Fortnite"). Letta una volta e cancellata.
// Stringa vuota se il file non c'è.
std::string takeAutostart();

}  // namespace xc::app
