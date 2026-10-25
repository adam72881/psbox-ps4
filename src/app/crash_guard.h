// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Notices when the app went down. On the PS4 a CE-34878-0 often kills the
// process without delivering SIGSEGV, so the signal handler never runs.
// Every flushed log line rewrites the marker with that line; the next start
// reads it. A fatal signal still overwrites the marker with the signal name.
#pragma once

#include <cstdint>
#include <string>

namespace xc::app::crash {

// SIGTERM/INT/QUIT/HUP bloccati prima di ogni thread, così lo eredita
// il processo. Il thread sigwait li prende. Va chiamato per primo in main.
void blockCloseSignals();
// Handlers for the fatal signals and std::terminate, writing to `markerPath`.
void install(const std::string& markerPath);
// What the app is doing (copied; up to 95 bytes): "home", "streaming ...".
void setStage(const char* stage);
// Log file descriptor. The fatal-signal handler writes RIP and the fault
// address here with write+fsync (no malloc). -1 disables that copy.
void setLogFd(int fd);
// Last flushed log line, kept in the marker even if no signal arrives.
void noteProgress(const char* line);
// The main loop is alive. startWatchdog(): a thread that writes a "stalled"
// marker after `stallMs` without a heartbeat, and removes it if the loop
// comes back (a long hang, not a crash).
void heartbeat();
// Thread video vivo (chiamato dal loop di decode). Se resta fermo >3 s
// con lo stream attivo, il watchdog scrive lo stato nel log.
void videoHeartbeat();
void setStreamingWatch(bool on);
void startWatchdog(uint32_t stallMs);

struct Previous {
    bool found = false;
    std::string kind;   // "signal 11 (SIGSEGV)", "last line", "uncaught exception", "stalled"
    std::string stage;  // setStage() or the last log line
};
// The last session's marker, if it left one (then removed).
Previous takePrevious(const std::string& markerPath);

}  // namespace xc::app::crash
