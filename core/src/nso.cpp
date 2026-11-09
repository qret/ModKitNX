// NSO0: разбор, сегменты, образ, символы. Порт zbic_nso.read_header, check_nso.Nso, nso_min.NSO.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/nso.hpp"
#include "mk/zbic.hpp"

#include <cstring>
#include <lz4.h>

namespace mk {

bool parse_nso_header(const uint8_t* d, size_t n, NsoHeader& h) {
    if (n < kNsoHeaderSize || std::memcmp(d, "NSO0", 4) != 0) return false;
    h.flags = rd32(d + 0x0C);
    for (int i = 0; i < 3; i++) {
        NsoSegment& s = h.seg[i];
        s.file_offset = rd32(d + 0x10 + i * 0x10);
        s.memory_offset = rd32(d + 0x14 + i * 0x10);
        s.size = rd32(d + 0x18 + i * 0x10);
        s.compressed_size = rd32(d + 0x60 + i * 4);
        s.compressed = (h.flags >> i) & 1;
        s.hashed = (h.flags >> (i + 3)) & 1;
        std::memcpy(s.hash.data(), d + 0xA0 + i * 0x20, 32);
    }
    std::memcpy(h.module_id.data(), d + 0x40, 32);
    h.bss_size = rd32(d + 0x3C);
    h.module_name_offset = rd32(d + 0x1C);
    h.module_name_size = rd32(d + 0x2C);
    return true;
}

Bytes lz4_block_decompress(const uint8_t* src, size_t n, size_t expected) {
    Bytes out(expected);
    if (n > 0x7FFFFFFF || expected > 0x7FFFFFFF) throw Error("NSO_LZ4", "сегмент LZ4 слишком велик");
    int got = LZ4_decompress_safe(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(out.data()),
                                  int(n), int(expected));
    if (got < 0) throw Error("NSO_LZ4", "сегмент LZ4 не распаковался (битые данные)");
    if (size_t(got) != expected)
        throw Error("NSO_LZ4", "сегмент LZ4: распаковано " + std::to_string(got) + " байт, а в заголовке " +
                                   std::to_string(expected));
    return out;
}

static const char* kSegName[3] = {"text", "rodata", "data"};

Bytes nso_segment(const Bytes& raw, const NsoHeader& h, int n, IHash* verify) {
    const NsoSegment& s = h.seg[n];
    size_t fo = std::min<size_t>(s.file_offset, raw.size());
    size_t len = std::min<size_t>(s.compressed_size, raw.size() - fo);
    Bytes data;
    if (s.compressed) {
        if (h.zbic()) data = zbic_decompress(raw.data() + fo, len, s.size);
        else data = lz4_block_decompress(raw.data() + fo, len, s.size);
    } else {
        size_t take = std::min<size_t>(len, s.size);
        data.assign(raw.begin() + fo, raw.begin() + fo + take);
        if (data.size() != s.size)
            throw Error("NSO_BAD", std::string("сегмент ") + kSegName[n] + ": в файле меньше байт, чем в заголовке");
    }
    if (verify && s.hashed) {
        Digest d = verify->sha256(data.data(), data.size());
        if (d != s.hash)
            throw Error("NSO_HASH", std::string("сегмент ") + kSegName[n] + ": sha256 не сошлась с заголовком NSO");
    }
    return data;
}

NsoImage::NsoImage(const Bytes& raw, IHash* verify) {
    if (!parse_nso_header(raw.data(), raw.size(), header)) throw Error("NSO_BAD", "нет магии NSO0");
    Bytes* segs[3] = {&text, &ro, &data};
    for (int i = 0; i < 3; i++) {
        *segs[i] = nso_segment(raw, header, i, verify);
        // плоский образ как check_nso.Nso.image
        uint32_t mo = header.seg[i].memory_offset, sz = header.seg[i].size;
        if (flat.size() < mo) flat.resize(mo, 0);
        if (flat.size() < size_t(mo) + sz) flat.resize(size_t(mo) + sz, 0);
        std::memcpy(flat.data() + mo, segs[i]->data(), sz);
    }
    // .dynamic по способу nso_min (через mem)
    if (text.size() < 8) throw Error("NSO_BAD", "text короче 8 байт");
    mod_off_ = rd32(text.data() + 4);
    Bytes m = mem(mod_off_, 8);
    if (m.size() < 8 || std::memcmp(m.data(), "MOD0", 4) != 0) throw Error("NSO_BAD", "не найден MOD0");
    dyn_addr_ = uint64_t(mod_off_) + int64_t(int32_t(rd32(m.data() + 4)));
    for (uint64_t a = dyn_addr_;; a += 16) {
        Bytes e = mem(a, 16);
        if (e.size() < 16) throw Error("NSO_BAD", ".dynamic обрезан");
        int64_t tag = int64_t(rd64(e.data()));
        uint64_t val = rd64(e.data() + 8);
        if (tag == 0) break;
        dt[tag].push_back(val);
    }
    for (int t : {5, 10, 6, 11})
        if (!dt.count(t)) throw Error("NSO_BAD", "в .dynamic нет DT_" + std::to_string(t));
}

Bytes NsoImage::mem(uint64_t a, size_t n) const {
    const NsoSegment* s = header.seg;
    const Bytes* b[3] = {&text, &ro, &data};
    for (int i = 0; i < 3; i++) {
        uint64_t lo = s[i].memory_offset, hi = lo + s[i].size;
        if (a >= lo && a < hi) {
            size_t off = size_t(a - lo);
            size_t take = std::min(n, b[i]->size() - off);
            return Bytes(b[i]->begin() + off, b[i]->begin() + off + take);
        }
    }
    return Bytes(n, 0);   // bss и вне образа — нули (как nso_min)
}

std::vector<NsoSymbol> NsoImage::syms_min() const {
    uint64_t strtab = dt.at(5)[0], strsz = dt.at(10)[0], symtab = dt.at(6)[0], syment = dt.at(11)[0];
    auto str = [&](uint64_t o) {
        Bytes b = mem(strtab + o, 1024);
        size_t i = 0;
        while (i < b.size() && b[i]) i++;
        if (i == b.size()) i = b.empty() ? 0 : b.size() - 1;   // b[:-1] при find() == -1, как в nso_min
        return std::string(b.begin(), b.begin() + i);
    };
    uint64_t nsyms;
    auto g = dt.find(0x6ffffef5);
    if (g == dt.end()) {
        if (!syment) throw Error("NSO_BAD", "DT_SYMENT = 0");
        nsyms = (strtab - symtab) / syment;
    } else {
        uint64_t ga = g->second[0];
        Bytes hh = mem(ga, 16);
        if (hh.size() < 16) throw Error("NSO_BAD", "GNU hash обрезан");
        uint32_t nbuck = rd32(hh.data()), symoff = rd32(hh.data() + 4), maskwords = rd32(hh.data() + 8);
        uint64_t bucket_off = ga + 16 + uint64_t(maskwords) * 8;
        Bytes bk = mem(bucket_off, size_t(nbuck) * 4);
        if (bk.size() < size_t(nbuck) * 4) throw Error("NSO_BAD", "GNU hash: корзины обрезаны");
        uint32_t last = symoff;
        if (nbuck) {
            last = 0;
            for (uint32_t i = 0; i < nbuck; i++) last = std::max(last, rd32(bk.data() + 4 * i));
        }
        if (last < symoff) {
            nsyms = symoff;
        } else {
            uint64_t chain = bucket_off + uint64_t(nbuck) * 4;
            uint64_t i = last;
            for (uint64_t guard = 0;; guard++) {
                Bytes v = mem(chain + (i - symoff) * 4, 4);
                if (v.size() < 4 || guard > (1u << 24)) throw Error("NSO_BAD", "GNU hash: цепочка обрезана");
                i++;
                if (rd32(v.data()) & 1) break;
            }
            nsyms = i;
        }
    }
    std::vector<NsoSymbol> out;
    out.reserve(size_t(nsyms));
    for (uint64_t i = 0; i < nsyms; i++) {
        Bytes e = mem(symtab + i * syment, 24);
        if (e.size() < 24) throw Error("NSO_BAD", ".dynsym обрезан");
        NsoSymbol s;
        uint32_t nameo = rd32(e.data());
        s.info = e[4];
        s.shndx = uint16_t(e[6] | e[7] << 8);
        s.value = rd64(e.data() + 8);
        s.size = rd64(e.data() + 16);
        s.name = nameo < strsz ? str(nameo) : std::string();
        out.push_back(std::move(s));
    }
    return out;
}

std::map<std::string, NsoSymbol> NsoImage::exports() const {
    std::map<std::string, NsoSymbol> m;
    for (auto& s : syms_min())
        if (s.shndx != 0 && !s.name.empty()) m[s.name] = s;
    return m;
}

std::map<std::string, NsoSymbol> NsoImage::imports() const {
    std::map<std::string, NsoSymbol> m;
    for (auto& s : syms_min())
        if (s.shndx == 0 && !s.name.empty()) m[s.name] = s;
    return m;
}

// check_nso.Nso: образ из сегментов по memory_offset, .dynamic читается из образа.
NsoFlat parse_nso_flat(const Bytes& raw) {
    NsoFlat n;
    if (!parse_nso_header(raw.data(), raw.size(), n.header)) throw Error("NSO_BAD", "нет магии NSO0");
    for (int i = 0; i < 3; i++) {
        const NsoSegment& s = n.header.seg[i];
        size_t fo = std::min<size_t>(s.file_offset, raw.size());
        size_t len = std::min<size_t>(s.compressed_size, raw.size() - fo);
        Bytes seg;
        if (s.compressed) seg = n.header.zbic() ? zbic_decompress(raw.data() + fo, len, s.size)
                                                : lz4_block_decompress(raw.data() + fo, len, s.size);
        else seg.assign(raw.begin() + fo, raw.begin() + fo + len);
        size_t take = std::min<size_t>(seg.size(), s.size);   // seg[:size]
        if (n.image.size() < s.memory_offset) n.image.resize(s.memory_offset, 0);
        // image[moff:moff+size] = seg[:size] (срез Python: длина может измениться)
        size_t end = std::min<size_t>(n.image.size(), size_t(s.memory_offset) + s.size);
        n.image.erase(n.image.begin() + s.memory_offset, n.image.begin() + end);
        n.image.insert(n.image.begin() + s.memory_offset, seg.begin(), seg.begin() + take);
    }
    const Bytes& im = n.image;
    if (im.size() < 8) throw Error("NSO_BAD", "образ короче 8 байт");
    uint32_t mo = rd32(im.data() + 4);
    if (size_t(mo) + 8 > im.size() || std::memcmp(im.data() + mo, "MOD0", 4) != 0)
        throw Error("NSO_BAD", "не найден MOD0");
    int64_t dyn = int64_t(mo) + int32_t(rd32(im.data() + mo + 4));
    for (int64_t o = dyn;; o += 16) {
        if (o < 0 || size_t(o) + 16 > im.size()) throw Error("NSO_BAD", ".dynamic за образом");
        int64_t t = int64_t(rd64(im.data() + o));
        uint64_t v = rd64(im.data() + o + 8);
        if (t == 0) break;
        if (t != 1) n.tags[t] = v;
    }
    return n;
}

// check_nso.Nso.symbols(): число символов — DT_HASH (nchain), иначе до начала .dynstr;
// строка без NUL — исключение (как image.index). Возвращает имена импортов.
std::vector<std::string> nso_flat_imports(const NsoFlat& n) {
    const Bytes& im = n.image;
    for (int t : {5, 6, 10})
        if (!n.tags.count(t)) throw Error("NSO_BAD", "нет DT_" + std::to_string(t));
    uint64_t strtab = n.tags.at(5), symtab = n.tags.at(6), strsz = n.tags.at(10);
    uint64_t count = 0;
    if (n.tags.count(4)) {
        uint64_t p = n.tags.at(4) + 4;
        if (p + 4 > im.size()) throw Error("NSO_BAD", "DT_HASH за образом");
        count = rd32(im.data() + p);
    }
    uint64_t end = count ? symtab + count * 24 : strtab;
    std::vector<std::string> out;
    for (uint64_t o = symtab; o + 24 <= end && o + 24 <= im.size(); o += 24) {
        uint32_t name = rd32(im.data() + o);
        uint16_t shndx = uint16_t(im[o + 6] | im[o + 7] << 8);
        if (name >= strsz) continue;
        uint64_t st = strtab + name;
        if (st > im.size()) throw Error("NSO_BAD", "строка за образом");
        const uint8_t* b = im.data() + st;
        const void* z = std::memchr(b, 0, im.size() - st);
        if (!z) throw Error("NSO_BAD", "строка без NUL");
        std::string nm(reinterpret_cast<const char*>(b), static_cast<const uint8_t*>(z) - b);
        if (nm.empty()) continue;
        if (shndx == 0) out.push_back(nm);
    }
    return out;
}

std::optional<std::string> NsoImage::module_name() const {
    if (ro.size() < 8) return std::nullopt;
    uint32_t w0 = rd32(ro.data()), w1 = rd32(ro.data() + 4);
    size_t a, len;
    if (w0 == 0) { a = 8; len = w1; }
    else if (w0 == 1 && ro.size() >= 0x14) { a = 0x14; len = rd32(ro.data() + 0x10); }
    else return std::nullopt;
    a = std::min(a, ro.size());
    len = std::min(len, ro.size() - a);
    std::string s(ro.begin() + a, ro.begin() + a + len);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    size_t k = s.find_last_of("\\/");
    if (k != std::string::npos) s = s.substr(k + 1);
    s = s.substr(0, s.find('.'));
    if (s.empty()) return std::nullopt;
    return s;
}

std::optional<std::array<int, 3>> NsoImage::mw_version() const {
    size_t start = ro.size() > 0x4000 ? ro.size() - 0x4000 : 0;
    const uint8_t* t = ro.data() + start;
    size_t n = ro.size() - start;
    static const char pre[] = "SDK MW+Nintendo+Nintendo";
    const size_t pl = sizeof(pre) - 1;
    static const char* var[4] = {"SDK", "Sdk", "SDk", "SdK"};
    std::optional<std::array<int, 3>> best;
    auto digits = [&](size_t& p, long long& v) {
        size_t s = p;
        v = 0;
        while (p < n && t[p] >= '0' && t[p] <= '9') { v = v * 10 + (t[p] - '0'); if (v > 1000000000) v = 1000000000; p++; }
        return p > s;
    };
    size_t pos = 0;
    while (pos + pl <= n) {
        size_t p = pos;
        bool ok = std::memcmp(t + p, pre, pl) == 0;
        if (ok) {
            p += pl;
            ok = false;
            for (auto v : var)
                if (p + 4 <= n && std::memcmp(t + p, v, 3) == 0 && t[p + 3] == '_') { ok = true; break; }
            if (ok) {
                p += 4;
                size_t s = p;
                while (p < n && t[p] != '-' && t[p] != 0 && p - s < 64) p++;
                ok = p > s && p < n && t[p] == '-';
                if (ok) {
                    p++;
                    long long a, b, c;
                    ok = digits(p, a) && p < n && t[p] == '_';
                    if (ok) { p++; ok = digits(p, b) && p < n && t[p] == '_'; }
                    if (ok) { p++; ok = digits(p, c); }
                    if (ok) {
                        std::array<int, 3> v{int(a), int(b), int(c)};
                        if (!best || v > *best) best = v;
                        pos = p;
                        continue;
                    }
                }
            }
        }
        pos++;
    }
    return best;
}

std::string version_text(const std::array<int, 3>& v) {
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
}

}  // namespace mk
