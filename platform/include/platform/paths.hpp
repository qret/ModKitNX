// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — пути на карте. Имя приложения приходит из сборки (project.cmake: APP_NAME → -DMK_APP_NAME).
#pragma once

#include <string>

// CMakeLists.txt передаёт APP_NAME / APP_VERSION из project.cmake (одно место).
#if defined(APP_NAME)
#define MK_APP_NAME APP_NAME
#else
#define MK_APP_NAME "ModKitNX"   // запасное значение для проверки синтаксиса без сборки
#endif
#if defined(APP_VERSION)
#define MK_APP_VERSION APP_VERSION
#else
#define MK_APP_VERSION "0.0.0"
#endif

namespace mk::plat::paths {

inline const std::string kSdRoot = "sdmc:/";
inline const std::string kAppDir = std::string("sdmc:/switch/") + MK_APP_NAME + "/";
inline const std::string kLogsDir = kAppDir + "logs/";
inline const std::string kModsDir = kAppDir + "mods/";
inline const std::string kSdkDirOwn = kAppDir + "sdk/";                  // основное место sdk
inline const std::string kSdkDirDbi = "sdmc:/switch/DBI/dumps/SDK/";     // запасное место sdk
// Свой .nro на постоянном месте (обновление из приложения кладёт его сюда же). Ярлык на домашнем экране (Р-43)
// запускает именно его; kAppNroForwarder — тот же путь в виде, который передаёт sphaira (без «sdmc:»): от него
// считается TitleID ярлыка (mk::forwarder::title_id), и он же лежит в /nextNroPath.
inline const std::string kAppNro = kAppDir + MK_APP_NAME + ".nro";
inline const std::string kAppNroForwarder = std::string("/switch/") + MK_APP_NAME + "/" + MK_APP_NAME + ".nro";
inline const std::string kSettingsFile = kAppDir + "settings.ini";   // прежнее место языка (lang=ru|en), только чтение
inline const std::string kConfigFile = kAppDir + "config.json";      // {"language": "uk"} — язык интерфейса
inline const std::string kConsolePassport = kAppDir + "console-passport.txt";
inline const std::string kProdKeys = "sdmc:/switch/prod.keys";          // только проверка наличия
inline const std::string kContentsDir = "sdmc:/atmosphere/contents/";
inline const std::string kCrashReportsDir = "sdmc:/atmosphere/crash_reports/";   // отчёты Atmosphère о падении
inline const std::string kRomfsModules = "romfs:/modules/";
// Кэш показа (mk::cache, core/include/mk/cache.hpp): расходный, удалить можно — проверится заново.
inline const std::string kCacheDir = kAppDir + "cache/";
inline const std::string kGamesCache = kCacheDir + "games.json";
inline const std::string kSdkFilesCache = kCacheDir + "sdk-files.json";
inline const std::string kIconsCacheDir = kCacheDir + "icons/";       // <TID>.jpg
inline const std::string kGameCheckCacheDir = kCacheDir + "game/";    // <TID>.json

// sdmc:/atmosphere/contents/<TID>/ — TID 16 hex, заглавные (как в квитанции формата 1).
std::string contentsDirFor(unsigned long long titleId);
std::string tidHex(unsigned long long titleId);

}  // namespace mk::plat::paths
