// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — журнал в sdmc:/switch/<APP_NAME>/logs/<APP_NAME>-ГГГГММДД-ЧЧММСС.log.
// Каждый запуск — свой файл; хранятся последние kKeepFiles (старые свои журналы приложение убирает само).
// Журнал включается в «Настройках» (config.json app_log); ПО УМОЛЧАНИЮ ВЫКЛЮЧЕН.
// Правило: ключи и их фрагменты в журнал не пишутся никогда.
#pragma once

#include <string>

namespace mk::plat::log {

constexpr int kKeepFiles = 10;
// Открывает файл журнала (создаёт папку), если журнал включён в настройках. Без карты/при отказе — журнал молчит,
// приложение работает.
void open();
// Включить/выключить журнал сейчас и запомнить выбор (config.json). false — не удалось записать настройку.
bool setEnabled(bool on);
bool enabled();
void close();
// printf-формат; строка дополняется временем и '\n'. Потокобезопасно.
void line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Путь текущего файла журнала ("" если не открыт) — показывается на экране итога.
std::string currentPath();

}  // namespace mk::plat::log

#define MK_LOG(...) ::mk::plat::log::line(__VA_ARGS__)
