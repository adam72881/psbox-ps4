// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#pragma once

#include <cstdarg>

namespace xc::log {

enum class Level { Debug, Info, Warn, Error };

void setLevel(Level level);
// Also appends every line to this file (created if missing). nullptr disables.
void setFile(const char* path);

void write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace xc::log

// Usata dagli shim C (pthread). Scrive e svuota il file subito.
extern "C" void xc_log_flush_line(const char* level, const char* msg);
/* Come sopra, ma senza localtime/strftime: la prima istruzione di un thread
 * nuovo non deve toccare il TLS a fs:0-8. */
extern "C" void xc_log_raw(const char* msg);
/* write+fsync di una riga corta, senza mutex, senza noteProgress e senza
 * il frame da 4 KB di write(): serve nel punto in cui il log normale muore. */
extern "C" void xc_log_stamp(const char* msg);
/* 1 dopo setFile: gli shim non scrivono durante l'init statica. */
extern "C" int xc_log_is_ready(void);

#define XC_LOGD(...) ::xc::log::write(::xc::log::Level::Debug, __VA_ARGS__)
#define XC_LOGI(...) ::xc::log::write(::xc::log::Level::Info, __VA_ARGS__)
#define XC_LOGW(...) ::xc::log::write(::xc::log::Level::Warn, __VA_ARGS__)
#define XC_LOGE(...) ::xc::log::write(::xc::log::Level::Error, __VA_ARGS__)
