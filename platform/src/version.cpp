// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/utils/switch_utils.h (SdkVersion::parse, parseString)
#include "platform/version.hpp"

#include <cctype>
#include <cstdio>

namespace mk::plat {

std::string Version3::toString() const {
    if (!valid) return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", unsigned(major), unsigned(minor), unsigned(micro));
    return buf;
}

int Version3::compare(const Version3& o) const {
    if (!valid || !o.valid) return 0;
    if (major != o.major) return major < o.major ? -1 : 1;
    if (minor != o.minor) return minor < o.minor ? -1 : 1;
    if (micro != o.micro) return micro < o.micro ? -1 : 1;
    return 0;
}

Version3 Version3::fromSdkAddon(uint32_t val) {
    Version3 v;
    if (val == 0) return v;
    v.major = static_cast<uint8_t>((val >> 24) & 0xFF);
    v.minor = static_cast<uint8_t>((val >> 16) & 0xFF);
    v.micro = static_cast<uint8_t>((val >> 8) & 0xFF);
    v.rel = static_cast<uint8_t>(val & 0xFF);
    v.valid = true;
    return v;
}

Version3 Version3::fromString(const std::string& s) {
    Version3 v;
    unsigned nums[3] = {0, 0, 0};
    int got = 0;
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    while (i < s.size() && got < 3) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) break;
        unsigned n = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            n = n * 10 + unsigned(s[i] - '0');
            if (n > 255) return Version3{};
            ++i;
        }
        nums[got++] = n;
        if (i < s.size() && (s[i] == '.' || s[i] == '_')) ++i;
        else break;
    }
    if (got < 2) return v;
    v.major = static_cast<uint8_t>(nums[0]);
    v.minor = static_cast<uint8_t>(nums[1]);
    v.micro = static_cast<uint8_t>(nums[2]);
    v.valid = true;
    return v;
}

Version3 Version3::make(uint8_t a, uint8_t b, uint8_t c) {
    Version3 v;
    v.major = a;
    v.minor = b;
    v.micro = c;
    v.valid = true;
    return v;
}

}  // namespace mk::plat
