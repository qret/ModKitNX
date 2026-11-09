// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/paths.hpp"

#include <cstdio>

#include "platform/service_lock.hpp"

namespace mk::plat {

namespace paths {

std::string tidHex(unsigned long long titleId) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llX", titleId);
    return buf;
}

std::string contentsDirFor(unsigned long long titleId) { return kContentsDir + tidHex(titleId) + "/"; }

}  // namespace paths

std::recursive_mutex& serviceMutex() {
    static std::recursive_mutex m;
    return m;
}

}  // namespace mk::plat
