// SPDX-License-Identifier: GPL-3.0-or-later
// НЕ ИСПОЛЬЗУЕТСЯ: тот же разбор есть в ядре (mk::parse_nacp, core/include/mk/nacp.hpp) — одно
// место правила, сверяется с эталоном. Файл компилируется глобом, но не вызывается (ui/README.md); оставлен,
// пока не решено, убирать ли его из сборки.
// Based on TorrentShopNX by Langegen (GPL-3.0): source/utils/nacp_text.cpp
//
// Переделано: пространство имён mk::plat::nacp, имена функций; логика распаковки и проверки чистоты текста —
// как в оригинале (сырой DEFLATE через zlib, inflateInit2(-MAX_WBITS), строгая проверка результата).
#include "platform/nacp_text.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace mk::plat::nacp {

namespace {

size_t boundedLen(const char* s, size_t maxLen) {
    const void* nul = std::memchr(s, 0, maxLen);
    return nul ? static_cast<size_t>(static_cast<const char*>(nul) - s) : maxLen;
}

// Длина верной UTF-8-последовательности с позиции i; 0 — неверная.
size_t utf8SeqLen(const unsigned char* s, size_t i, size_t len) {
    const unsigned char c = s[i];
    size_t n = 0;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    else return 0;
    if (i + n > len) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((s[i + k] & 0xC0) != 0x80) return 0;
    }
    const unsigned char c1 = s[i + 1];
    if (c == 0xE0 && c1 < 0xA0) return 0;   // избыточная запись
    if (c == 0xED && c1 >= 0xA0) return 0;  // суррогаты UTF-16
    if (c == 0xF0 && c1 < 0x90) return 0;   // избыточная запись
    if (c == 0xF4 && c1 >= 0x90) return 0;  // выше U+10FFFF
    return n;
}

// Поле — чистый текст с NUL внутри поля: верный UTF-8, без управляющих символов.
bool isCleanField(const char* field, size_t size) {
    const size_t len = boundedLen(field, size);
    if (len == size) return false;
    const auto* s = reinterpret_cast<const unsigned char*>(field);
    for (size_t i = 0; i < len;) {
        if (s[i] < 0x20 || s[i] == 0x7F) return false;
        const size_t n = utf8SeqLen(s, i, len);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

}  // namespace

std::string sanitizeUtf8(const char* str, size_t maxLen) {
    std::string out;
    if (!str) return out;
    const size_t len = boundedLen(str, maxLen);
    const auto* s = reinterpret_cast<const unsigned char*>(str);
    out.reserve(len);
    for (size_t i = 0; i < len;) {
        if (s[i] < 0x20 || s[i] == 0x7F) {
            if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') out.push_back(' ');
            ++i;
            continue;
        }
        const size_t n = utf8SeqLen(s, i, len);
        if (n == 0) {
            ++i;
            continue;
        }
        out.append(str + i, n);
        i += n;
    }
    return out;
}

std::string fieldText(const char* field, size_t size) {
    if (!field || !isCleanField(field, size)) return "";
    return std::string(field, boundedLen(field, size));
}

bool unpackTitles(void* table, size_t tableSize) {
    if (!table || tableSize < kLangEntrySize + 2) return false;
    const auto* src = static_cast<const unsigned char*>(table);
    const size_t compSize = static_cast<size_t>(src[0]) | (static_cast<size_t>(src[1]) << 8);
    if (compSize == 0 || compSize > tableSize - 2) return false;

    // Место с запасом: у новых прошивок языков больше, оставляем только первые tableSize байт.
    std::vector<unsigned char> out(tableSize * 4);
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(src + 2);
    zs.avail_in = static_cast<uInt>(compSize);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&zs, Z_FINISH);
    const size_t consumed = zs.total_in;
    const size_t produced = zs.total_out;
    inflateEnd(&zs);

    // Обычная таблица почти никогда не образует DEFLATE-поток, который кончается ровно на заявленной длине
    // и раскрывается в целые записи чистого текста.
    if (rc != Z_STREAM_END || consumed != compSize) return false;
    if (produced == 0 || produced % kLangEntrySize != 0) return false;
    bool anyName = false;
    for (size_t off = 0; off < produced; off += kLangEntrySize) {
        const char* entry = reinterpret_cast<const char*>(out.data() + off);
        if (!isCleanField(entry, kLangNameSize) ||
            !isCleanField(entry + kLangNameSize, kLangEntrySize - kLangNameSize)) {
            return false;
        }
        if (entry[0] != '\0') anyName = true;
    }
    if (!anyName) return false;

    std::memset(table, 0, tableSize);
    size_t keep = produced < tableSize ? produced : tableSize;
    keep -= keep % kLangEntrySize;
    std::memcpy(table, out.data(), keep);
    return true;
}

}  // namespace mk::plat::nacp
