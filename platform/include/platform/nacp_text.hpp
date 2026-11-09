// SPDX-License-Identifier: GPL-3.0-or-later
// НЕ ИСПОЛЬЗУЕТСЯ: тот же разбор есть в ядре (mk::parse_nacp, core/include/mk/nacp.hpp) — одно
// место правила, сверяется с эталоном. Файл компилируется глобом, но не вызывается (ui/README.md); оставлен,
// пока не решено, убирать ли его из сборки.
// Based on TorrentShopNX by Langegen (GPL-3.0): source/utils/nacp_text.h
//
// Названия и версия из NACP. У игр SDK 20+ таблица названий (16 × 0x300 байт с начала NACP) может быть
// сжата: 2 байта длины (LE), затем «сырой» DEFLATE, который раскрывается в обычные записи по 0x300 байт.
// libnx её не распаковывает — без этого вместо названия новых игр получается мусор.
#pragma once

#include <cstddef>
#include <string>

namespace mk::plat::nacp {

constexpr size_t kTitleTableSize = 0x3000;  // 16 языков × 0x300
constexpr size_t kLangEntrySize = 0x300;    // имя 0x200 + издатель 0x100
constexpr size_t kLangNameSize = 0x200;

// Распаковывает сжатую таблицу на месте. true — была сжата и теперь обычная; обычную не трогает.
bool unpackTitles(void* table, size_t tableSize = kTitleTableSize);

// Текст до первого NUL (не длиннее maxLen) без неверного UTF-8 и управляющих символов.
std::string sanitizeUtf8(const char* s, size_t maxLen);

// Поле NACP (имя, издатель), если это чистый текст; "" — если там двоичные данные.
std::string fieldText(const char* field, size_t size);

}  // namespace mk::plat::nacp
