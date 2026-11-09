// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — перевод mk::zbic_need для экрана. См. platform/zbic_policy.hpp.
#include "platform/zbic_policy.hpp"

#include <mk/zbic.hpp>

namespace mk::plat {

ZbicNeed zbicPolicy(const Version3& firmware, bool amsKnown, const Version3& atmosphere) {
    const std::array<int, 3> hos{firmware.major, firmware.minor, firmware.micro};
    const std::array<int, 3> ams{atmosphere.major, atmosphere.minor, atmosphere.micro};
    const bool amsOk = amsKnown && atmosphere.valid;
    // zbic_in_exefs = true: спрашиваем само правило; сжата ли игра — отдельный вопрос (zbic_scan).
    const mk::ZbicNeed n = mk::zbic_need(firmware.valid ? &hos : nullptr, amsOk ? &ams : nullptr, true);
    if (n.verdict == "not_needed_hos22") return ZbicNeed::NotNeededHos22;
    if (n.verdict == "not_needed_ams112") return ZbicNeed::NotNeededAms112;
    return amsOk ? ZbicNeed::Needed : ZbicNeed::NeededAmsUnknown;
}

}  // namespace mk::plat
