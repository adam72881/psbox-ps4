// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Aggiornamento da /data/psbox/update/: un PSBox-PS4-*.pkg copiato via FTP.
// All'avvio e da Impostazioni si propone «C'è un aggiornamento, vuoi
// installarlo?» Sì/No. Mai automatico. Su Sì: verifica (magic, size stabile,
// SFO più nuovo), copia in /data/pkg, install tramite GoldHEN (RPI 127.0.0.1:12800
// o BinLoader :9090). Mai sceAppInstUtilAppInstallPkg / BGFT dal processo
// dell'app (01.43: 0x80020012 danneggiava l'installazione, BGFT 0x80990088).
#pragma once

#include <string>

namespace xc::app {

enum class LocalUpdateStatus { None, Available, Installed, Failed };

struct LocalUpdateResult {
    LocalUpdateStatus status = LocalUpdateStatus::None;
    std::string version;   // versione del pkg (es. 1.0.44)
    std::string path;      // path /data/... del pkg
    std::string detail;    // toast / log
};

// Solo scan: nessuna installazione, nessun rename/unlink. Se c'è un pkg più
// nuovo → Available (il chiamante mostra il dialogo Sì/No).
LocalUpdateResult findLocalUpdate(const std::string& current);

// Dopo Sì: BGFT FORCE_UPDATE con contentUrl /user/data/... sul file già in
// /data/..., attende progress, unlink solo a OK, LoadExec("exit").
LocalUpdateResult installLocalUpdate(const std::string& current);

inline LocalUpdateResult applyLocalUpdate(const std::string& current) {
    return installLocalUpdate(current);
}

}  // namespace xc::app
