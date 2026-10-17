// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Aggiornamento da /data/psbox/update/: un PSBox-PS4-*.pkg copiato via FTP.
// Nessuna rete. Si controlla all'avvio e dal pulsante in Impostazioni.
#pragma once

#include <string>

namespace xc::app {

enum class LocalUpdateStatus { None, Installed, Failed };

struct LocalUpdateResult {
    LocalUpdateStatus status = LocalUpdateStatus::None;
    std::string version;  // la versione trovata, se c'è un pkg
    std::string detail;   // riga già scritta nel log, per il toast
};

// `current` è XC_APP_VERSION ("1.0.29-ps4"). Se il pkg è più nuovo lo sposta
// in /data/pkg, avvia BGFT (FORCE_UPDATE) o AppInstallPkg senza overwrite,
// poi prova a riaprire. Non chiama PrepareOverwritePkg e non disinstalla.
LocalUpdateResult applyLocalUpdate(const std::string& current);

}  // namespace xc::app
