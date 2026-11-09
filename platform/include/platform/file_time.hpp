// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — дата изменения файла на карте (для выбора последнего отчёта Atmosphère о падении, mk::logs).
// На консоли — fsFsGetFileTimeStampRaw по ФС устройства sdmc (метка FAT/exFAT), запасной путь — stat();
// на ПК (предпросмотр) — stat(). Нет даты — nullopt: ядро тогда выбирает по числу в начале имени.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mk::plat {

std::optional<int64_t> fileMtime(const std::string& path);

}  // namespace mk::plat
