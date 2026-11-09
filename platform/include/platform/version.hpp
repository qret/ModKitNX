// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/utils/switch_utils.h (struct SdkVersion)
//
// Версия вида major.minor.micro: sdk_addon_version из заголовка NCA (байты b3.b2.b1, b0 — rel),
// версия прошивки, версия nnSdk из строки «SDK MW … 19_3_13». Без libnx — собирается и на ПК.
#pragma once

#include <cstdint>
#include <string>

namespace mk::plat {

struct Version3 {
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t micro = 0;
    uint8_t rel = 0;
    bool valid = false;

    std::string toString() const;

    // Сравнение по трём полям; с невалидной стороной сравнение ложно (как в оригинале).
    int compare(const Version3& o) const;  // -1 / 0 / 1; при невалидной стороне — 0
    bool operator<(const Version3& o) const { return valid && o.valid && compare(o) < 0; }
    bool operator>(const Version3& o) const { return valid && o.valid && compare(o) > 0; }
    bool operator==(const Version3& o) const { return valid && o.valid && compare(o) == 0; }

    // sdk_addon_version (@0x21C заголовка NCA): b3=major, b2=minor, b1=micro, b0=rel. 0 — нет версии.
    static Version3 fromSdkAddon(uint32_t v);
    // «19.3.13», «19_3_13», «v19.3.13» — первые три числа через «.» или «_».
    static Version3 fromString(const std::string& s);
    static Version3 make(uint8_t a, uint8_t b, uint8_t c);
};

}  // namespace mk::plat
