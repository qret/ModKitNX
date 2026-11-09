// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — настройки приложения: sdmc:/switch/<APP>/config.json (одно место для всех настроек).
//
//   {"language": "uk", "theme": "system", "update_check": true, "update_skip": "0.2.0"}
//
// Переносимый код (stdio + nlohmann), без libnx: тот же файл читают Switch-сборка и предпросмотр на ПК.
// Нет файла или он битый — значения по умолчанию; запись меняет одно поле, прочие сохраняются.
#pragma once

#include <string>

namespace mk::plat::config {

// Ключи config.json (одно место).
inline const char* const kLanguage = "language";        // код языка ("en-US", "ru", "uk"); нет — первый запуск
inline const char* const kTheme = "theme";              // "system" | "light" | "dark"; нет — "system"
inline const char* const kUpdateCheck = "update_check"; // проверять обновление при запуске; нет — true
inline const char* const kAppLog = "app_log";            // вести журнал приложения (logs/<APP>-*.log); нет — false (по умолчанию выключен)
inline const char* const kUpdateSkip = "update_skip";   // версия, которую пользователь пропустил ("0.2.0")

std::string getString(const std::string& key, const std::string& def = "");
bool getBool(const std::string& key, bool def);

// Записать одно поле. Запись через <файл>.part → переименование; прежний файл остаётся как <файл>.prev
// (одна копия: rename на карте Switch не перезаписывает, поэтому прежняя .prev сначала убирается).
// false — карта не записалась (прежний файл не тронут).
bool setString(const std::string& key, const std::string& value);
bool setBool(const std::string& key, bool value);

// Путь файла (по умолчанию paths::kConfigFile). Для хост-тестов и предпросмотра.
void setPathForTests(const std::string& path);

}  // namespace mk::plat::config
