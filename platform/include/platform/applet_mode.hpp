// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — режим тайтла.
// Текстовая заглушка — идея NX-torrent-player (Apache-2.0, app/main.cpp appletModeBlock). Код свой.
#pragma once

namespace mk::plat {

// true — запущены как Application/SystemApplication (полная память, все службы).
bool isTitleMode();
// Тип апплета числом (для паспорта и журнала).
int appletTypeNumber();

// ЗАПАСНОЙ путь: основной экран апплета — borealis (ui/applet_activity);
// этот текстовый экран через consoleInit (без GPU) — только если borealis не поднялся. Ждёт «+» и возвращается.
// Шрифт консоли libnx — только ASCII, поэтому русский текст дан транслитом (кириллицы в нём нет).
void runAppletModeBlock();

}  // namespace mk::plat
