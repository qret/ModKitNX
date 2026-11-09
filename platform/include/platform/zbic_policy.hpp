// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — нужно ли снимать сжатие ZBIC. ПРАВИЛО — ОДНО МЕСТО, в ядре: mk::zbic_need (core/include/mk/zbic.hpp):
//   HOS ≥ 22.0.0 → не нужно; HOS < 22 и Atmosphère ≥ 1.12.0 → не нужно; иначе (Atmosphère < 1.12.0 или не
//   известна) → нужно, делается по умолчанию. Вопроса пользователю нет —
//   версии консоль знает сама (set:sys, spl ExosphereApiVersion). Здесь — только перевод ответа ядра для экрана.
#pragma once

#include "platform/version.hpp"

namespace mk::plat {

enum class ZbicNeed {
    NotNeededHos22,   // HOS ≥ 22
    NotNeededAms112,  // Atmosphère ≥ 1.12.0
    Needed,           // Atmosphère < 1.12.0
    NeededAmsUnknown  // версия Atmosphère не прочитана
};

ZbicNeed zbicPolicy(const Version3& firmware, bool amsKnown, const Version3& atmosphere);
inline bool zbicUnpackNeeded(ZbicNeed n) { return n == ZbicNeed::Needed || n == ZbicNeed::NeededAmsUnknown; }

}  // namespace mk::plat
