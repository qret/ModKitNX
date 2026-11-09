// ModKitNX core — NCA3 / PFS0 / билет. См. mk/nca.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/nca.hpp"

#include "mk/aes128.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mk::nca {

namespace {

constexpr uint32_t kMagicNca3 = 0x3341434E;  // "NCA3"
constexpr uint32_t kMagicPfs0 = 0x30534650;  // "PFS0"

std::string num(uint64_t v) { return std::to_string(v); }

std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

bool anyNonZero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i]) return true;
    return false;
}

}  // namespace

Header parse_header(const uint8_t* dec, size_t n, IHash& hash) {
    if (n < kHeaderSize) throw Error("NCA_HEADER", "заголовок NCA короче 0xC00", {num(n)});
    if (rd32(dec + 0x200) != kMagicNca3) throw Error("NCA_HEADER", "нет магии NCA3 (ключ заголовка не тот?)", {});
    Header h;
    h.content_type = dec[0x205];
    h.key_generation_old = dec[0x206];
    h.kaek_index = dec[0x207];
    h.content_size = rd64(dec + 0x208);
    h.program_id = rd64(dec + 0x210);
    h.sdk_addon = rd32(dec + 0x21C);
    h.key_generation = dec[0x220];
    std::memcpy(h.rights_id.data(), dec + 0x230, 16);
    h.has_rights_id = anyNonZero(dec + 0x230, 16);
    for (int i = 0; i < 4; ++i) std::memcpy(h.key_area[i].data(), dec + 0x300 + i * 0x10, 16);

    for (int i = 0; i < 4; ++i) {
        FsSection& s = h.sections[i];
        s.index = i;
        const uint8_t* e = dec + 0x240 + i * 0x10;
        const uint64_t start = rd32(e), end = rd32(e + 4);
        if (end <= start) continue;
        s.present = true;
        s.offset = start * kMediaUnit;
        s.size = (end - start) * kMediaUnit;
        const uint8_t* fsh = dec + 0x400 + i * 0x200;
        const Digest d = hash.sha256(fsh, 0x200);
        if (std::memcmp(d.data(), dec + 0x280 + i * 0x20, 32) != 0) {
            throw Error("NCA_FS_HASH", "хеш заголовка секции NCA не сошёлся", {num(i)});
        }
        s.fs_type = fsh[0x2];
        s.hash_type = fsh[0x3];
        s.encryption_type = fsh[0x4];
        for (int k = 0; k < 8; ++k) s.ctr_upper[k] = fsh[0x140 + 7 - k];
        if (s.hash_type == kHashSha256) {
            // HierarchicalSha256: +0x8 master hash 0x20, +0x28 block size, +0x2C число слоёв, +0x30 слои {off,size}.
            s.pfs0_offset = rd64(fsh + 0x40);
            s.pfs0_size = rd64(fsh + 0x48);
        }
        s.has_sparse = anyNonZero(fsh + 0x148, 0x30);
        s.has_compression = anyNonZero(fsh + 0x178, 0x28);
    }
    return h;
}

const FsSection& exefs_section(const Header& h) {
    if (h.content_type != kContentProgram) {
        throw Error("NCA_NOT_PROGRAM", "NCA не программный (тип контента)", {num(h.content_type)});
    }
    const FsSection& s = h.sections[0];
    if (!s.present || s.fs_type != kFsPartition || s.hash_type != kHashSha256 || s.pfs0_size == 0 ||
        s.pfs0_offset + s.pfs0_size > s.size) {
        throw Error("NCA_NO_EXEFS", "в секции 0 нет exefs (PFS0)",
                    {num(s.present), num(s.fs_type), num(s.hash_type)});
    }
    if (s.encryption_type != kEncCtr && s.encryption_type != kEncNone && s.encryption_type != kEncCtrSkipHash) {
        throw Error("NCA_EXEFS_CRYPTO", "шифрование секции exefs не AES-CTR", {num(s.encryption_type)});
    }
    if (s.has_sparse || s.has_compression) {
        throw Error("NCA_EXEFS_LAYOUT", "секция exefs разреженная или сжатая — не поддержано",
                    {num(s.has_sparse), num(s.has_compression)});
    }
    return s;
}

std::array<uint8_t, 16> ctr_for(const FsSection& s, uint64_t abs_offset) {
    std::array<uint8_t, 16> c{};
    std::memcpy(c.data(), s.ctr_upper.data(), 8);
    uint64_t v = abs_offset >> 4;
    for (int i = 15; i >= 8; --i) {
        c[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
    return c;
}

std::array<uint8_t, 16> decrypt_key_area_entry(const Header& h, int index, const uint8_t kaek[16]) {
    std::array<uint8_t, 16> out{};
    aes::ecb_decrypt(kaek, h.key_area.at(static_cast<size_t>(index)).data(), out.data(), 16);
    return out;
}

CtrFn soft_ctr(const uint8_t key[16]) {
    std::array<uint8_t, 16> k{};
    std::memcpy(k.data(), key, 16);
    return [k](const uint8_t ctr[16], uint8_t* data, size_t n) { aes::ctr_crypt(k.data(), ctr, data, data, n); };
}

// ---------------- SectionReader
SectionReader::SectionReader(RawReadFn raw, CtrFn ctr, const FsSection& s)
    : raw_(std::move(raw)), ctr_(std::move(ctr)), s_(s) {}

void SectionReader::read(uint64_t rel, uint8_t* out, size_t n) const {
    if (n == 0) return;
    if (rel > s_.size || n > s_.size - rel) {
        throw Error("NCA_RANGE", "чтение за концом секции NCA", {hex64(rel), num(n), hex64(s_.size)});
    }
    constexpr size_t kChunk = 0x40000;   // 256 КБ за раз
    Bytes buf;
    while (n > 0) {
        const uint64_t abs = s_.offset + rel;
        const uint64_t a0 = abs & ~uint64_t(0xF);
        const size_t lead = static_cast<size_t>(abs - a0);
        const size_t take = std::min(n, kChunk - lead);
        const size_t span = (lead + take + 0xF) & ~size_t(0xF);
        buf.resize(span);
        raw_(a0, buf.data(), span);
        if (s_.encryption_type != kEncNone) {
            const auto c = ctr_for(s_, a0);
            ctr_(c.data(), buf.data(), span);
        }
        std::memcpy(out, buf.data() + lead, take);
        out += take;
        rel += take;
        n -= take;
    }
}

// ---------------- PFS0
const Pfs0Entry* Pfs0::find(const std::string& name) const {
    for (const auto& e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

uint64_t pfs0_header_size(const uint8_t* p) {
    if (rd32(p) != kMagicPfs0) throw Error("NCA_PFS0_MAGIC", "нет магии PFS0 в exefs (ключ секции не тот?)", {});
    const uint64_t count = rd32(p + 4), strtab = rd32(p + 8);
    if (count > 0x1000 || strtab > 0x100000) {
        throw Error("NCA_PFS0_FORMAT", "заголовок PFS0 неправдоподобен", {num(count), num(strtab)});
    }
    return 0x10 + count * 0x18 + strtab;
}

Pfs0 parse_pfs0(const uint8_t* p, size_t n, uint64_t data_size) {
    if (n < 0x10) throw Error("NCA_PFS0_FORMAT", "заголовок PFS0 обрезан", {num(n)});
    Pfs0 r;
    r.header_size = pfs0_header_size(p);
    if (n < r.header_size) throw Error("NCA_PFS0_FORMAT", "заголовок PFS0 обрезан", {num(n)});
    const uint32_t count = rd32(p + 4), strtab = rd32(p + 8);
    const uint8_t* names = p + 0x10 + size_t(count) * 0x18;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = p + 0x10 + size_t(i) * 0x18;
        Pfs0Entry en;
        en.offset = rd64(e);
        en.size = rd64(e + 8);
        const uint32_t no = rd32(e + 0x10);
        if (no >= strtab) throw Error("NCA_PFS0_FORMAT", "имя файла PFS0 за таблицей строк", {num(i)});
        const char* s = reinterpret_cast<const char*>(names + no);
        const size_t maxLen = strtab - no;
        const size_t len = strnlen(s, maxLen);
        if (len == 0 || len == maxLen) throw Error("NCA_PFS0_FORMAT", "имя файла PFS0 пустое или без конца", {num(i)});
        en.name.assign(s, len);
        if (en.name.find('/') != std::string::npos || en.name == "." || en.name == "..") {
            throw Error("NCA_PFS0_FORMAT", "недопустимое имя файла PFS0", {num(i)});
        }
        if (en.offset > data_size || en.size > data_size - en.offset) {
            throw Error("NCA_PFS0_FORMAT", "файл PFS0 за границей данных", {en.name});
        }
        r.entries.push_back(std::move(en));
    }
    return r;
}

ExefsReader::ExefsReader(const SectionReader& r) : r_(r) {
    const FsSection& s = r.section();
    uint8_t first[0x10];
    r.read(s.pfs0_offset, first, sizeof(first));
    const uint64_t hs = pfs0_header_size(first);
    if (hs > s.pfs0_size) throw Error("NCA_PFS0_FORMAT", "заголовок PFS0 больше секции", {num(hs)});
    Bytes hdr(static_cast<size_t>(hs));
    r.read(s.pfs0_offset, hdr.data(), hdr.size());
    pfs_ = parse_pfs0(hdr.data(), hdr.size(), s.pfs0_size - hs);
}

void ExefsReader::read(const Pfs0Entry& e, uint64_t off, uint8_t* out, size_t n) const {
    if (off > e.size || n > e.size - off) {
        throw Error("NCA_RANGE", "чтение за концом файла exefs", {e.name, hex64(off), num(n)});
    }
    r_.read(r_.section().pfs0_offset + pfs_.header_size + e.offset + off, out, n);
}

// ---------------- билет
Ticket parse_ticket(const uint8_t* p, size_t n) {
    if (n < 4) throw Error("TICKET_FORMAT", "билет короче 4 байт", {num(n)});
    Ticket t;
    t.sig_type = rd32(p);
    size_t sig = 0, pad = 0;
    switch (t.sig_type) {
        case 0x10000: case 0x10003: sig = 0x200; pad = 0x3C; break;   // RSA-4096
        case 0x10001: case 0x10004: sig = 0x100; pad = 0x3C; break;   // RSA-2048
        case 0x10002: case 0x10005: sig = 0x3C; pad = 0x40; break;    // ECDSA
        case 0x10006: sig = 0x14; pad = 0x28; break;                  // HMAC
        default: throw Error("TICKET_FORMAT", "неизвестный тип подписи билета", {hex64(t.sig_type)});
    }
    const size_t base = 4 + sig + pad;
    if (n < base + 0x180) throw Error("TICKET_FORMAT", "билет обрезан", {num(n)});
    const uint8_t* c = p + base;
    std::memcpy(t.enc_titlekey.data(), c + 0x40, 16);
    t.titlekey_type = c[0x141];
    t.key_generation = c[0x145];
    std::memcpy(t.rights_id.data(), c + 0x160, 16);
    return t;
}

std::string rights_id_hex(const std::array<uint8_t, 16>& r) { return hex(r.data(), r.size(), true); }

}  // namespace mk::nca
