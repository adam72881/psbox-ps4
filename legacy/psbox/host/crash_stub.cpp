// SPDX-License-Identifier: GPL-3.0-or-later
// Host stubs for crash_guard symbols referenced by util/log.cpp.
#include "app/crash_guard.h"

namespace xc::app::crash {
void setLogFd(int) {}
void noteProgress(const char*) {}
}  // namespace xc::app::crash
