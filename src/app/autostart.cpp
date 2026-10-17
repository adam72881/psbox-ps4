// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/autostart.h"

#include "platform/platform.h"
#include "util/log.h"

#include <unistd.h>

namespace xc::app {

std::string takeAutostart() {
    std::string path = platform::dataDir() + "/autostart.txt";
    std::string text;
    if (!platform::readFile(path, text)) return {};
    ::unlink(path.c_str());
    size_t end = text.find_first_of("\r\n");
    std::string line = text.substr(0, end);
    size_t begin = 0;
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    line.erase(0, begin);
    XC_LOGI("autostart letto e cancellato: %s", line.c_str());
    return line;
}

}  // namespace xc::app
