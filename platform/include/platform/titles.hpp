// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.cpp (сканирование установленных игр,
// saveInstalledIcon), source/utils/switch_utils.cpp (queryInstalledGame)
//
// Список установленных игр (ns), название/версия/иконка из control data. Блокирующие IPC — звать из фонового
// потока, не из UI-потока.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mk::plat {

struct TitleControl {
    uint64_t appId = 0;
    std::string name;            // правило языка: American English → British English → первый непустой
    std::string publisher;
    std::string displayVersion;  // NACP display_version, «1.0.3»
    std::vector<uint8_t> iconJpeg;  // JPEG из control data (иконка меню HOME), пусто — нет
    bool titlesWereCompressed = false;  // таблица названий SDK 20+ была сжата и распакована
};

// Идентификаторы всех записей приложений (nsListApplicationRecord, пачками по 50).
bool listApplicationIds(std::vector<uint64_t>& out);

// Control data одной игры (NsApplicationControlSource_Storage).
bool readTitleControl(uint64_t appId, TitleControl& out);

}  // namespace mk::plat
