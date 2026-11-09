// NACP, сжатая таблица названий (SDK 20+), свой распаковщик DEFLATE, sdk_addon_version NCA,
// безопасное имя папки.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Формат сжатой таблицы узнан из TorrentShopNX by Langegen (GPL-3.0): source/utils/nacp_text.cpp
// (u16 LE длина + raw DEFLATE; распаковывается в записи по 0x300; проверка «чистого» текста).
// Код ниже свой. Выбор языка — как эталонный parse_nacp (сверка — tests/host/compare_pc.py).
#include "mk/nacp.hpp"

#include <cstring>

namespace mk {

// ============================================================ DEFLATE (RFC 1951), декодер
namespace {

struct BitIn {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool over = false;
    int bit() {
        if (cnt == 0) {
            if (pos >= n) { over = true; return 0; }
            buf = p[pos++];
            cnt = 8;
        }
        int b = buf & 1;
        buf >>= 1;
        cnt--;
        return b;
    }
    uint32_t bits(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) v |= uint32_t(bit()) << i;
        return v;
    }
};

struct Huff {
    uint16_t count[16];
    uint16_t sym[320];
};

// Каноническое дерево по длинам кодов. false — лишние коды (переполнение).
bool build(Huff& h, const uint8_t* len, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    if (h.count[0] == n) return true;   // пустое дерево — допустимо (например, нет расстояний)
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = uint16_t(offs[l] + h.count[l]);
    for (int i = 0; i < n; i++)
        if (len[i]) h.sym[offs[len[i]]++] = uint16_t(i);
    return true;
}

int decode(BitIn& in, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= in.bit();
        int c = h.count[l];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
        if (in.over) return -1;
    }
    return -1;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(BitIn& in, Bytes& out, size_t max_out, const Huff& lh, const Huff& dh) {
    for (;;) {
        int s = decode(in, lh);
        if (s < 0 || in.over) return false;
        if (s < 256) {
            if (out.size() >= max_out) return false;
            out.push_back(uint8_t(s));
        } else if (s == 256) {
            return true;
        } else {
            s -= 257;
            if (s >= 29) return false;
            size_t len = kLenBase[s] + in.bits(kLenExtra[s]);
            int d = decode(in, dh);
            if (d < 0 || d >= 30 || in.over) return false;
            size_t dist = kDistBase[d] + in.bits(kDistExtra[d]);
            if (dist > out.size() || out.size() + len > max_out) return false;
            size_t from = out.size() - dist;
            for (size_t i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
}

}  // namespace

bool inflate_raw(const uint8_t* src, size_t n, Bytes& out, size_t max_out, size_t* consumed) {
    BitIn in{src, n};
    out.clear();
    static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int last;
    do {
        last = in.bit();
        int type = int(in.bits(2));
        if (in.over) return false;
        if (type == 0) {
            in.cnt = 0;   // к границе байта
            if (in.pos + 4 > n) return false;
            uint16_t len = uint16_t(src[in.pos] | src[in.pos + 1] << 8);
            uint16_t nlen = uint16_t(src[in.pos + 2] | src[in.pos + 3] << 8);
            in.pos += 4;
            if (uint16_t(~nlen) != len || in.pos + len > n || out.size() + len > max_out) return false;
            out.insert(out.end(), src + in.pos, src + in.pos + len);
            in.pos += len;
        } else if (type == 1) {
            static Huff lh, dh;
            static bool ready = false;
            if (!ready) {
                uint8_t l[288];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                build(lh, l, 288);
                uint8_t d[30];
                for (auto& x : d) x = 5;
                build(dh, d, 30);
                ready = true;
            }
            if (!codes(in, out, max_out, lh, dh)) return false;
        } else if (type == 2) {
            int nlen = int(in.bits(5)) + 257, ndist = int(in.bits(5)) + 1, ncode = int(in.bits(4)) + 4;
            if (nlen > 286 || ndist > 30) return false;
            uint8_t lens[320] = {0};
            for (int i = 0; i < ncode; i++) lens[ord[i]] = uint8_t(in.bits(3));
            Huff ch;
            if (!build(ch, lens, 19)) return false;
            int idx = 0;
            uint8_t ll[320] = {0};
            while (idx < nlen + ndist) {
                int s = decode(in, ch);
                if (s < 0 || in.over) return false;
                if (s < 16) {
                    ll[idx++] = uint8_t(s);
                } else {
                    int rep, val = 0;
                    if (s == 16) {
                        if (idx == 0) return false;
                        val = ll[idx - 1];
                        rep = 3 + int(in.bits(2));
                    } else if (s == 17) {
                        rep = 3 + int(in.bits(3));
                    } else {
                        rep = 11 + int(in.bits(7));
                    }
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) ll[idx++] = uint8_t(val);
                }
            }
            if (ll[256] == 0) return false;
            Huff lh, dh;
            if (!build(lh, ll, nlen) || !build(dh, ll + nlen, ndist)) return false;
            if (!codes(in, out, max_out, lh, dh)) return false;
        } else {
            return false;
        }
    } while (!last);
    if (consumed) *consumed = in.pos;
    return true;
}

// ============================================================ UTF-8 и текст NACP
namespace {

size_t bounded(const char* s, size_t m) {
    const void* z = std::memchr(s, 0, m);
    return z ? size_t(static_cast<const char*>(z) - s) : m;
}

size_t seq_len(const unsigned char* s, size_t i, size_t len) {
    unsigned char c = s[i];
    size_t n;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    else return 0;
    if (i + n > len) return 0;
    for (size_t k = 1; k < n; k++)
        if ((s[i + k] & 0xC0) != 0x80) return 0;
    unsigned char c1 = s[i + 1];
    if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90))
        return 0;
    return n;
}

bool clean_field(const char* f, size_t size) {
    size_t len = bounded(f, size);
    if (len == size) return false;
    auto* s = reinterpret_cast<const unsigned char*>(f);
    for (size_t i = 0; i < len;) {
        if (s[i] < 0x20 || s[i] == 0x7F) return false;
        size_t n = seq_len(s, i, len);
        if (!n) return false;
        i += n;
    }
    return true;
}

// Декодирование UTF-8 с заменой битых последовательностей на U+FFFD (errors="replace").
std::u32string decode_utf8(const uint8_t* s, size_t n) {
    std::u32string out;
    for (size_t i = 0; i < n;) {
        size_t k = seq_len(s, i, n);
        if (!k) { out.push_back(0xFFFD); i++; continue; }
        char32_t c;
        if (k == 1) c = s[i];
        else if (k == 2) c = char32_t(s[i] & 0x1F) << 6 | (s[i + 1] & 0x3F);
        else if (k == 3) c = char32_t(s[i] & 0x0F) << 12 | char32_t(s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F);
        else c = char32_t(s[i] & 0x07) << 18 | char32_t(s[i + 1] & 0x3F) << 12 | char32_t(s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F);
        out.push_back(c);
        i += k;
    }
    return out;
}

std::string encode_utf8(const std::u32string& u) {
    std::string o;
    for (char32_t c : u) {
        if (c < 0x80) o.push_back(char(c));
        else if (c < 0x800) { o.push_back(char(0xC0 | c >> 6)); o.push_back(char(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) {
            o.push_back(char(0xE0 | c >> 12)); o.push_back(char(0x80 | (c >> 6 & 0x3F))); o.push_back(char(0x80 | (c & 0x3F)));
        } else {
            o.push_back(char(0xF0 | c >> 18)); o.push_back(char(0x80 | (c >> 12 & 0x3F)));
            o.push_back(char(0x80 | (c >> 6 & 0x3F))); o.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return o;
}

// str.isspace() Python
bool py_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::u32string py_strip(const std::u32string& s) {
    size_t a = 0, b = s.size();
    while (a < b && py_space(s[a])) a++;
    while (b > a && py_space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// _cstr эталона: до NUL, decode("utf-8", "replace"), strip()
std::string cstr(const uint8_t* p, size_t n) {
    size_t len = bounded(reinterpret_cast<const char*>(p), n);
    return encode_utf8(py_strip(decode_utf8(p, len)));
}

const char* kLangs[16] = {"AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish",
                          "Spanish", "Italian", "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean",
                          "TraditionalChinese", "SimplifiedChinese", "BrazilianPortuguese"};

}  // namespace

std::string sanitize_utf8(const char* str, size_t max_len) {
    std::string out;
    if (!str) return out;
    size_t len = bounded(str, max_len);
    auto* s = reinterpret_cast<const unsigned char*>(str);
    for (size_t i = 0; i < len;) {
        if (s[i] < 0x20 || s[i] == 0x7F) {
            if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') out.push_back(' ');
            i++;
            continue;
        }
        size_t n = seq_len(s, i, len);
        if (!n) { i++; continue; }
        out.append(str + i, n);
        i += n;
    }
    return out;
}

bool unpack_nacp_titles(uint8_t* table, size_t size) {
    if (!table || size < kNacpLangEntry + 2) return false;
    size_t comp = size_t(table[0]) | size_t(table[1]) << 8;
    if (comp == 0 || comp > size - 2) return false;
    Bytes out;
    size_t used = 0;
    if (!inflate_raw(table + 2, comp, out, size * 4, &used)) return false;
    if (used != comp || out.empty() || out.size() % kNacpLangEntry) return false;
    bool any = false;
    for (size_t off = 0; off < out.size(); off += kNacpLangEntry) {
        const char* e = reinterpret_cast<const char*>(out.data() + off);
        if (!clean_field(e, kNacpLangName) || !clean_field(e + kNacpLangName, kNacpLangEntry - kNacpLangName)) return false;
        if (e[0]) any = true;
    }
    if (!any) return false;
    std::memset(table, 0, size);
    size_t keep = std::min(out.size(), size);
    keep -= keep % kNacpLangEntry;
    std::memcpy(table, out.data(), keep);
    return true;
}

NacpInfo parse_nacp(const uint8_t* data, size_t size) {
    if (size < kNacpDisplayVersion + 0x10) throw Error("NACP_SHORT", "control.nacp короче ожидаемого");
    NacpInfo r;
    Bytes tbl(data, data + kNacpTitleTable);
    r.table_was_compressed = unpack_nacp_titles(tbl.data(), tbl.size());
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> titles;
    for (int i = 0; i < 16; i++) {
        const uint8_t* rec = tbl.data() + i * kNacpLangEntry;
        std::string name = cstr(rec, kNacpLangName), pub = cstr(rec + kNacpLangName, kNacpLangEntry - kNacpLangName);
        if (!name.empty()) titles.push_back({kLangs[i], {name, pub}});
    }
    const std::pair<std::string, std::string>* pick = nullptr;
    for (const char* k : {"AmericanEnglish", "BritishEnglish"})
        for (auto& t : titles)
            if (!pick && t.first == k) pick = &t.second;
    if (!pick && !titles.empty()) pick = &titles[0].second;
    if (pick) {
        r.title = pick->first;
        if (!pick->second.empty()) r.publisher = pick->second;
        for (auto& t : titles)
            if (t.second == *pick) { r.title_lang = t.first; break; }
    }
    std::string dv = cstr(data + kNacpDisplayVersion, 0x10);
    if (!dv.empty()) r.display_version = dv;
    for (auto& t : titles) r.languages.push_back(t.first);
    return r;
}

// ============================================================ NCA
std::optional<uint32_t> nca_sdk_addon_version(const uint8_t* hdr, size_t n) {
    if (n < 0x220) return std::nullopt;
    if (std::memcmp(hdr + 0x200, "NCA3", 4) != 0 && std::memcmp(hdr + 0x200, "NCA2", 4) != 0) return std::nullopt;
    return rd32(hdr + 0x21C);
}

SdkAddonVersion SdkAddonVersion::from_u32(uint32_t v) {
    SdkAddonVersion s;
    s.major = int(v >> 24 & 0xFF);
    s.minor = int(v >> 16 & 0xFF);
    s.micro = int(v >> 8 & 0xFF);
    s.rel = int(v & 0xFF);
    return s;
}

std::string SdkAddonVersion::text() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(micro);
}

// ============================================================ имя папки (safety.safe_folder_name)
std::string safe_folder_name(const std::string& name, const std::string& fallback) {
    std::u32string u = decode_utf8(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    for (auto& c : u)
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*')
            c = ' ';
    std::u32string s;
    for (size_t i = 0; i < u.size();) {   // re.sub(r"\s+", " ")
        if (py_space(u[i])) {
            while (i < u.size() && py_space(u[i])) i++;
            s.push_back(' ');
        } else {
            s.push_back(u[i++]);
        }
    }
    auto rstrip_dot_space = [](std::u32string& x) {
        while (!x.empty() && (x.back() == '.' || x.back() == ' ')) x.pop_back();
    };
    s = py_strip(s);
    rstrip_dot_space(s);
    if (s.size() > 80) {
        s.resize(80);
        rstrip_dot_space(s);
    }
    bool all_dots = !s.empty() && s.find_first_not_of(U'.') == std::u32string::npos;
    if (s.empty() || all_dots) s = decode_utf8(reinterpret_cast<const uint8_t*>(fallback.data()), fallback.size());
    std::u32string head = py_strip(s.substr(0, s.find(U'.')));
    for (auto& c : head)
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    static const std::u32string reserved[] = {U"CON", U"PRN", U"AUX", U"NUL", U"CONIN$", U"CONOUT$",
                                              U"COM1", U"COM2", U"COM3", U"COM4", U"COM5", U"COM6", U"COM7", U"COM8", U"COM9",
                                              U"LPT1", U"LPT2", U"LPT3", U"LPT4", U"LPT5", U"LPT6", U"LPT7", U"LPT8", U"LPT9",
                                              U"COM¹", U"COM²", U"COM³", U"LPT¹", U"LPT²", U"LPT³"};
    for (auto& r : reserved)
        if (head == r) { s = U"_" + s; break; }
    return encode_utf8(s);
}

}  // namespace mk
