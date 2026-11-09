// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/utils/switch_utils.cpp (getConsoleSdkVersion)
//
// Переделано: кроме прошивки — Atmosphère (spl 65000), mesosphère (svcGetInfo 65000), emuMMC (spl 65007),
// режим, наличие ключей; запись паспорта.
#include "platform/console_info.hpp"

#include <switch.h>
#include <sys/stat.h>

#include <cstdio>
#include <ctime>
#include <mutex>

#include "platform/applet_mode.hpp"
#include "platform/keys.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "platform/service_lock.hpp"

namespace mk::plat {

namespace {

constexpr SplConfigItem kExosphereApiVersion = static_cast<SplConfigItem>(65000);
constexpr SplConfigItem kExosphereEmummcType = static_cast<SplConfigItem>(65007);  // [Г]
constexpr u32 kInfoTypeMesosphereMeta = 65000;
constexpr u64 kMesosphereMetaKernelVersion = 0;

std::once_flag g_once;
ConsoleInfo g_info;

void fill(ConsoleInfo& c) {
    ServiceLock lock(serviceMutex());

    SetSysFirmwareVersion fw{};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {  // set:sys поднимает borealis (switch_wrapper.c)
        c.firmware = Version3::make(fw.major, fw.minor, fw.micro);
        c.firmwareDisplay = fw.display_version;
    } else {
        const u32 hos = hosversionGet();
        if (hos) {
            c.firmware = Version3::make(HOSVER_MAJOR(hos), HOSVER_MINOR(hos), HOSVER_MICRO(hos));
            c.firmwareDisplay = c.firmware.toString();
        }
    }

    c.isAtmosphere = hosversionIsAtmosphere();

    if (R_SUCCEEDED(splInitialize())) {
        u64 v = 0;
        const Result rc = splGetConfig(kExosphereApiVersion, &v);
        c.amsRc = rc;
        if (R_SUCCEEDED(rc)) {
            c.amsKnown = true;
            c.atmosphere = Version3::make((v >> 56) & 0xFF, (v >> 48) & 0xFF, (v >> 40) & 0xFF);
            c.amsKeyGeneration = static_cast<uint8_t>((v >> 32) & 0xFF);
            const u32 tf = static_cast<u32>(v & 0xFFFFFFFFu);
            c.amsTargetFirmware = Version3::make((tf >> 24) & 0xFF, (tf >> 16) & 0xFF, (tf >> 8) & 0xFF);
        }
        u64 e = 0;
        if (R_SUCCEEDED(splGetConfig(kExosphereEmummcType, &e))) {
            c.emummcKnown = true;
            c.emummcType = e;
        }
        splExit();
    }

    u64 meta = 0;
    if (R_SUCCEEDED(svcGetInfo(&meta, kInfoTypeMesosphereMeta, 0, kMesosphereMetaKernelVersion))) {
        c.mesoKnown = true;
        c.mesoMeta = meta;
    }

    c.titleMode = isTitleMode();
    c.appletType = appletTypeNumber();
    c.prodKeysPresent = prodKeysPresent();
}

}  // namespace

ConsoleInfo queryConsole() {
    std::call_once(g_once, [] {
        fill(g_info);
        MK_LOG("console: fw %s, ams %s (known=%d, rc=0x%X, target %s), meso %d 0x%016lX, emummc %d/%lu, title=%d "
               "applet=%d keys=%d",
               g_info.firmware.toString().c_str(), g_info.atmosphere.toString().c_str(), g_info.amsKnown, g_info.amsRc,
               g_info.amsTargetFirmware.toString().c_str(), g_info.mesoKnown, g_info.mesoMeta, g_info.emummcKnown,
               static_cast<unsigned long>(g_info.emummcType), g_info.titleMode, g_info.appletType,
               g_info.prodKeysPresent);
    });
    return g_info;
}

std::string passportText(const ConsoleInfo& c, size_t sdkCount, size_t gameCount) {
    std::string t = "\xEF\xBB\xBF";
    char buf[256];
    auto line = [&](const std::string& s) { t += s + "\r\n"; };
    line(std::string(MK_APP_NAME) + " " + MK_APP_VERSION + " — паспорт консоли");
    line("прошивка (set:sys): " + (c.firmware.valid ? c.firmwareDisplay : std::string("не прочитана")));
    if (c.amsKnown) {
        std::snprintf(buf, sizeof(buf), "Atmosphère (spl 65000): %s, поколение ключа %u, целевая прошивка %s [раскладка — Г]",
                      c.atmosphere.toString().c_str(), unsigned(c.amsKeyGeneration),
                      c.amsTargetFirmware.toString().c_str());
        line(buf);
    } else {
        std::snprintf(buf, sizeof(buf), "Atmosphère (spl 65000): нет ответа, код 0x%X", c.amsRc);
        line(buf);
    }
    if (c.mesoKnown) {
        std::snprintf(buf, sizeof(buf), "ядро mesosphère (svcGetInfo 65000/0): 0x%016lX", c.mesoMeta);
    } else {
        std::snprintf(buf, sizeof(buf), "ядро mesosphère (svcGetInfo 65000/0): нет ответа");
    }
    line(buf);
    line(std::string("hosversionIsAtmosphere: ") + (c.isAtmosphere ? "да" : "нет"));
    if (c.emummcKnown) {
        std::snprintf(buf, sizeof(buf), "emuMMC (spl 65007) [Г]: %s (тип %lu)", c.emummcType ? "emuMMC" : "sysMMC",
                      static_cast<unsigned long>(c.emummcType));
        line(buf);
    }
    std::snprintf(buf, sizeof(buf), "режим: %s (тип апплета %d)", c.titleMode ? "тайтл" : "апплет", c.appletType);
    line(buf);
    line(std::string("prod.keys на карте: ") + (c.prodKeysPresent ? "есть" : "нет") + " (содержимое не читалось)");
    std::snprintf(buf, sizeof(buf), "найдено sdk: %zu, игр: %zu", sdkCount, gameCount);
    line(buf);
    return t;
}

bool writePassport(const std::string& text) {
    ::mkdir(paths::kAppDir.c_str(), 0777);
    const std::string part = paths::kConsolePassport + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok) return false;
    // Прежний паспорт откладывается, а не стирается.
    struct stat st;
    if (::stat(paths::kConsolePassport.c_str(), &st) == 0) {
        std::rename(paths::kConsolePassport.c_str(), (paths::kConsolePassport + ".prev-" + std::to_string(time(nullptr))).c_str());
    }
    return std::rename(part.c_str(), paths::kConsolePassport.c_str()) == 0;
}

}  // namespace mk::plat
