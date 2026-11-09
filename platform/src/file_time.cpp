// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — дата изменения файла. См. platform/file_time.hpp.
#include "platform/file_time.hpp"

#include <sys/stat.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace mk::plat {

std::optional<int64_t> fileMtime(const std::string& path) {
#if defined(__SWITCH__)
    // «sdmc:/a/b» -> ФС устройства sdmc и путь «/a/b».
    const size_t colon = path.find(":/");
    if (colon != std::string::npos) {
        const std::string dev = path.substr(0, colon);
        if (FsFileSystem* fs = fsdevGetDeviceFileSystem(dev.c_str())) {
            FsTimeStampRaw ts{};
            const std::string inner = path.substr(colon + 1);
            if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(fs, inner.c_str(), &ts)) && ts.is_valid && ts.modified)
                return static_cast<int64_t>(ts.modified);
        }
    }
#endif
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && st.st_mtime > 0) return static_cast<int64_t>(st.st_mtime);
    return std::nullopt;
}

}  // namespace mk::plat
