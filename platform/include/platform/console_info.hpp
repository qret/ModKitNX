// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — паспорт консоли: прошивка (set:sys), Atmosphère (exosphère через spl, mesosphère через svcGetInfo),
// тип emuMMC, режим тайтла/апплета, наличие ключей.
// Версия прошивки — способ TorrentShopNX (source/utils/switch_utils.cpp, getConsoleSdkVersion).
#pragma once

#include <cstdint>
#include <string>

#include "platform/version.hpp"

namespace mk::plat {

struct ConsoleInfo {
    // Прошивка (setsysGetFirmwareVersion; запасной путь — hosversionGet()).
    Version3 firmware;
    std::string firmwareDisplay;  // «19.0.1»

    // Atmosphère / exosphère: splGetConfig(65000 = ExosphereApiVersion).
    // [Г] раскладка (сверить по Atmosphère exosphere/program/source/smc/secmon_smc_info.cpp, GetExosphereApiVersion):
    //   биты 63..56 major, 55..48 minor, 47..40 micro, 39..32 поколение ключа, 31..0 целевая прошивка
    //   (ATMOSPHERE_TARGET_FIRMWARE: major<<24 | minor<<16 | micro<<8).
    bool amsKnown = false;
    Version3 atmosphere;
    uint8_t amsKeyGeneration = 0;
    Version3 amsTargetFirmware;
    uint32_t amsRc = 0;

    // mesosphère: svcGetInfo(InfoType 65000 = MesosphereMeta, 0, MesosphereMetaInfo_KernelVersion = 0) —
    // так же спрашивает модуль nxredirect (nxredirect/src/main.cpp, паспорт). Значение показываем сырым hex.
    bool mesoKnown = false;
    uint64_t mesoMeta = 0;

    bool isAtmosphere = false;    // hosversionIsAtmosphere()

    // [Г] splGetConfig(65007 = ExosphereEmummcType): 0 — sysMMC, иначе emuMMC.
    bool emummcKnown = false;
    uint64_t emummcType = 0;

    bool titleMode = false;
    int appletType = -1;
    bool prodKeysPresent = false;
};

ConsoleInfo queryConsole();  // кэшируется после первого вызова

// Текст паспорта (UTF-8 с BOM, CRLF — как паспорт модуля nxredirect) и запись в sdmc:/switch/<APP>/console-passport.txt.
std::string passportText(const ConsoleInfo& c, size_t sdkCount, size_t gameCount);
bool writePassport(const std::string& text);

}  // namespace mk::plat
