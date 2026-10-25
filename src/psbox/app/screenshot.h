// SPDX-License-Identifier: GPL-3.0-or-later
// Cattura frame VideoOut → PNG in /data/psbox/screenshots/ (solo menu / 1° frame stream).
#pragma once

#include <cstdint>
#include <string>

namespace xc::app::shot {

enum class Kind {
    Manual,
    SignIn,
    Loading,
    Library,
    GameDetails,
    Connecting,
    Settings,
    FirstStream,
    UpdatePrompt,  // dialogo «Vuoi installarlo?» Sì/No
};

// Opzione Settings (default off). Combo: tieni L1+R1+□ per 1 s (solo menu).
void setEnabled(bool on);
bool enabled();

// Copia il buffer già presentato (dopo draw + overlay) e scrive PNG in async.
// `allowStream`: solo FirstStream / false negli altri casi durante lo stream.
void request(Kind kind, bool allowStream = false);

// Poll dal main: combo L1+R1+□, prenota auto-shot al cambio schermo (se enabled).
// La cattura vera avviene in afterPresent() dopo drawRgba+present.
void pollMenu(bool l1, bool r1, bool square, bool inMenus, bool streaming, const char* screenTag);

// Chiamare sotto frameMutex subito dopo drawRgba()+present(): copia sync del
// buffer VideoOut appena flipato, poi encode PNG su thread dedicato.
void afterPresent();

}  // namespace xc::app::shot
