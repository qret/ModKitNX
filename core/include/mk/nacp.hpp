// NACP (control.nacp): название, издатель, display version; сжатая таблица названий SDK 20+;
// sdk_addon_version из расшифрованного заголовка NCA; безопасное имя папки.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Формат сжатой таблицы (u16 длина + raw DEFLATE, распаковывается в записи по 0x300)
// узнан из TorrentShopNX by Langegen (GPL-3.0): source/utils/nacp_text.cpp; код здесь свой,
// включая распаковщик DEFLATE (zlib не нужен). Правило выбора языка — как в эталонном parse_nacp (сверка — tests/host/compare_pc.py).
#pragma once

#include "mk/core.hpp"

#include <optional>

namespace mk {

constexpr size_t kNacpTitleTable = 0x3000, kNacpLangEntry = 0x300, kNacpLangName = 0x200;
constexpr size_t kNacpDisplayVersion = 0x3060;

struct NacpInfo {
    std::optional<std::string> title, publisher, title_lang, display_version;
    std::vector<std::string> languages;      // языки с непустым названием, в порядке NACP
    bool table_was_compressed = false;
};
// data — весь NACP (>= 0x3070). Сжатую таблицу распаковывает в копии. Error("NACP_SHORT").
NacpInfo parse_nacp(const uint8_t* data, size_t size);

// Распаковать сжатую таблицу на месте; false — таблица обычная (не тронута).
bool unpack_nacp_titles(uint8_t* table, size_t table_size = kNacpTitleTable);
// raw DEFLATE (RFC 1951). false — поток битый или длиннее max_out.
bool inflate_raw(const uint8_t* src, size_t n, Bytes& out, size_t max_out, size_t* consumed);
// Текст до NUL без битого UTF-8 и управляющих (\t\n\r -> пробел).
std::string sanitize_utf8(const char* s, size_t max_len);

// ------------------------------------------------------------ NCA
// sdk_addon_version (u32 @0x21C) из РАСШИФРОВАННОГО заголовка NCA (расшифровка — платформа).
// nullopt — нет магии NCA3/NCA2 по 0x200 или заголовок короче.
std::optional<uint32_t> nca_sdk_addon_version(const uint8_t* hdr, size_t n);
struct SdkAddonVersion {
    int major = 0, minor = 0, micro = 0, rel = 0;      // байты b3.b2.b1, b0
    static SdkAddonVersion from_u32(uint32_t v);
    std::array<int, 3> v3() const { return {major, minor, micro}; }
    std::string text() const;
};

// ------------------------------------------------------------ имена
// Имя папки, которое примут FAT/exFAT и Windows (правило safe_folder_name эталона).
std::string safe_folder_name(const std::string& name, const std::string& fallback = "game");

}  // namespace mk
