// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/keys.hpp"

#include <sys/stat.h>

#include "platform/paths.hpp"

namespace mk::plat {

bool prodKeysPresent() {
    struct stat st;
    return ::stat(paths::kProdKeys.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

}  // namespace mk::plat
