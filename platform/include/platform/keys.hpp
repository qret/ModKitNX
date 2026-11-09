// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — ключи консоли. Только проверка НАЛИЧИЯ sdmc:/switch/prod.keys, без чтения содержимого.
// Чтение (для NSP/NSZ на карте и запасного пути exefs) появится отдельной функцией: только в память,
// не в журнал, не в файлы, не на экран.
#pragma once

namespace mk::plat {

bool prodKeysPresent();

}  // namespace mk::plat
