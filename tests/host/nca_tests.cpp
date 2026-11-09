// ModKitNX — хост-тесты чтения exefs из сырого NCA (mk::nca, mk::aes) на синтетике.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NCA собирается здесь же: заголовок (без XTS — его снимает платформа), секция 0 PFS0 с хешами SHA-256,
// шифрование AES-CTR с выдуманными ключами (байты 0x10.., 0x20..). Настоящих ключей, игр и NCA в тестах нет.
// Негативные пробы обязаны краснеть.
#include "mk/aes128.hpp"
#include "mk/core.hpp"
#include "mk/nca.hpp"

#include <cstdio>
#include <cstring>
#include <map>

using namespace mk;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, what)                                                                 \
    do {                                                                                  \
        if (cond) { g_pass++; }                                                           \
        else { g_fail++; std::printf("  НЕТ %s (%s:%d)\n", what, __FILE__, __LINE__); } \
    } while (0)

template <class F> static bool throws(F f, const char* code = nullptr) {
    try {
        f();
    } catch (const Error& e) {
        if (code && e.code() != code) std::printf("    (код %s, ждали %s)\n", e.code().c_str(), code);
        return !code || e.code() == code;
    } catch (...) {
        return !code;
    }
    return false;
}

static Bytes from_hex(const char* h) {
    Bytes b;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) b.push_back(uint8_t(std::stoi(std::string(h + i, 2), nullptr, 16)));
    return b;
}

// ------------------------------------------------------------ AES: эталонные векторы
static void test_aes_vectors() {
    // FIPS-197, приложение C.1
    const Bytes k = from_hex("000102030405060708090a0b0c0d0e0f");
    const Bytes p = from_hex("00112233445566778899aabbccddeeff");
    const Bytes c = from_hex("69c4e0d86a7b0430d8cdb78070b4c55a");
    uint8_t o[16];
    aes::ecb_encrypt(k.data(), p.data(), o, 16);
    CHECK(std::memcmp(o, c.data(), 16) == 0, "AES-128: FIPS-197 C.1 шифрование");
    aes::ecb_decrypt(k.data(), c.data(), o, 16);
    CHECK(std::memcmp(o, p.data(), 16) == 0, "AES-128: FIPS-197 C.1 расшифровка");
    // NIST SP 800-38A F.5.1 (CTR-AES128), два блока
    const Bytes k2 = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
    const Bytes iv = from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const Bytes pt = from_hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    const Bytes ct = from_hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff");
    Bytes b(pt.size());
    aes::ctr_crypt(k2.data(), iv.data(), pt.data(), b.data(), pt.size());
    CHECK(b == ct, "AES-CTR: SP 800-38A F.5.1, перенос счётчика через байт 0xFF");
    // CTR с переносом через 64 бита: счётчик ...FFFFFFFF -> следующий блок совпадает с отдельным вызовом
    uint8_t ivA[16];
    std::memset(ivA, 0, 16);
    std::memset(ivA + 8, 0xFF, 8);
    Bytes z(32, 0), z2(16, 0), o2(32);
    aes::ctr_crypt(k2.data(), ivA, z.data(), o2.data(), 32);
    uint8_t ivB[16] = {0};
    ivB[7] = 1;
    aes::ctr_crypt(k2.data(), ivB, z2.data(), z2.data(), 16);
    CHECK(std::memcmp(o2.data() + 16, z2.data(), 16) == 0, "AES-CTR: перенос в старшие 8 байт счётчика");
}

// ------------------------------------------------------------ синтетический NCA
struct Synth {
    Bytes header;            // расшифрованный заголовок 0xC00
    Bytes body;              // весь NCA, тело секции зашифровано
    std::map<std::string, Bytes> files;
    uint8_t section_key[16];
    uint8_t kaek[16];
};

static Bytes make_pfs0(const std::map<std::string, Bytes>& files) {
    Bytes strtab;
    std::vector<std::pair<uint32_t, const Bytes*>> ent;
    for (const auto& f : files) {
        ent.push_back({uint32_t(strtab.size()), &f.second});
        strtab.insert(strtab.end(), f.first.begin(), f.first.end());
        strtab.push_back(0);
    }
    while (strtab.size() % 0x20) strtab.push_back(0);
    Bytes h(0x10 + ent.size() * 0x18, 0);
    wr32(h.data(), 0x30534650);
    wr32(h.data() + 4, uint32_t(ent.size()));
    wr32(h.data() + 8, uint32_t(strtab.size()));
    uint64_t off = 0;
    Bytes data;
    for (size_t i = 0; i < ent.size(); ++i) {
        uint8_t* e = h.data() + 0x10 + i * 0x18;
        const uint64_t sz = ent[i].second->size();
        for (int k = 0; k < 8; ++k) { e[k] = uint8_t(off >> (8 * k)); e[8 + k] = uint8_t(sz >> (8 * k)); }
        wr32(e + 0x10, ent[i].first);
        data.insert(data.end(), ent[i].second->begin(), ent[i].second->end());
        off += sz;
    }
    h.insert(h.end(), strtab.begin(), strtab.end());
    h.insert(h.end(), data.begin(), data.end());
    return h;
}

static void put64(uint8_t* p, uint64_t v) { for (int k = 0; k < 8; ++k) p[k] = uint8_t(v >> (8 * k)); }

static Synth make_nca(uint8_t enc_type = nca::kEncCtr, bool rights = false) {
    Synth s;
    for (int i = 0; i < 16; ++i) { s.section_key[i] = uint8_t(0x10 + i); s.kaek[i] = uint8_t(0x20 + i * 3); }
    Bytes npdm(0x80, 0);
    std::memcpy(npdm.data(), "META", 4);
    Bytes main(0x12345);
    for (size_t i = 0; i < main.size(); ++i) main[i] = uint8_t(i * 7 + (i >> 9));
    std::memcpy(main.data(), "NSO0", 4);
    Bytes sdk(0x3003, 0x5A);
    s.files = {{"main.npdm", npdm}, {"main", main}, {"sdk", sdk}};
    const Bytes pfs = make_pfs0(s.files);

    const uint64_t sec_off = 0xC00;            // 6 единиц по 0x200
    const uint64_t pfs_rel = 0x200;            // перед PFS0 — «таблица хешей» (здесь нули)
    uint64_t sec_size = pfs_rel + pfs.size();
    sec_size = (sec_size + 0x1FF) & ~uint64_t(0x1FF);

    s.header.assign(nca::kHeaderSize, 0);
    uint8_t* h = s.header.data();
    wr32(h + 0x200, 0x3341434E);
    h[0x204] = 0;            // distribution
    h[0x205] = 0;            // Program
    h[0x206] = 2;            // keygen old
    h[0x207] = 0;            // KAEK application
    put64(h + 0x208, sec_off + sec_size);
    put64(h + 0x210, 0x0100ABCD12340000ULL);
    wr32(h + 0x21C, 0x16020800);   // sdk 22.2.8
    h[0x220] = 0x13;
    if (rights) { h[0x230] = 0x01; h[0x23F] = 0x13; }
    wr32(h + 0x240, uint32_t(sec_off / 0x200));
    wr32(h + 0x244, uint32_t((sec_off + sec_size) / 0x200));
    // key area: [2] = ECB(kaek, section_key)
    aes::ecb_encrypt(s.kaek, s.section_key, h + 0x300 + 2 * 0x10, 16);
    uint8_t* fsh = h + 0x400;
    fsh[0] = 2;
    fsh[2] = nca::kFsPartition;
    fsh[3] = nca::kHashSha256;
    fsh[4] = enc_type;
    wr32(fsh + 0x8 + 0x20, 0x1000);      // block size
    wr32(fsh + 0x8 + 0x24, 2);           // слоёв
    put64(fsh + 0x30, 0);                // слой 0: таблица хешей
    put64(fsh + 0x38, 0x20);
    put64(fsh + 0x40, pfs_rel);          // слой 1: PFS0
    put64(fsh + 0x48, pfs.size());
    wr32(fsh + 0x140, 0x00000005);       // generation
    wr32(fsh + 0x144, 0xA1B2C3D4);       // secure value
    const Digest d = soft_hash().sha256(fsh, 0x200);
    std::memcpy(h + 0x280, d.data(), 32);

    s.body.assign(sec_off + sec_size, 0xEE);   // заголовок «как на диске» не нужен: платформа снимает XTS сама
    Bytes sec(sec_size, 0);
    std::memcpy(sec.data() + pfs_rel, pfs.data(), pfs.size());
    // шифруем секцию CTR по абсолютным смещениям
    nca::FsSection fs;
    fs.offset = sec_off;
    for (int k = 0; k < 8; ++k) fs.ctr_upper[k] = fsh[0x140 + 7 - k];
    const auto ctr = nca::ctr_for(fs, sec_off);
    aes::ctr_crypt(s.section_key, ctr.data(), sec.data(), sec.data(), sec.size());
    std::memcpy(s.body.data() + sec_off, sec.data(), sec.size());
    return s;
}

static nca::RawReadFn reader(const Bytes& body, int* calls = nullptr) {
    return [&body, calls](uint64_t off, uint8_t* out, size_t n) {
        if (calls) ++*calls;
        if (off > body.size() || n > body.size() - off) throw Error("RAW", "за концом", {});
        std::memcpy(out, body.data() + off, n);
    };
}

static void test_nca_exefs() {
    Synth s = make_nca();
    const nca::Header h = nca::parse_header(s.header.data(), s.header.size(), soft_hash());
    CHECK(h.program_id == 0x0100ABCD12340000ULL, "NCA: program id @0x210");
    CHECK(nca::effective_key_generation(h) == 0x13, "NCA: поколение ключа = max(@0x206, @0x220)");
    CHECK(!h.has_rights_id && h.kaek_index == 0, "NCA: без rights id, KAEK application");
    const nca::FsSection& sec = nca::exefs_section(h);
    CHECK(sec.offset == 0xC00 && sec.pfs0_offset == 0x200, "NCA: секция 0 и смещение PFS0 из HierarchicalSha256");
    // счётчик: старшие 8 байт = BE(secure_value<<32 | generation)
    const auto c = nca::ctr_for(sec, 0xC10);
    CHECK(c[0] == 0xA1 && c[3] == 0xD4 && c[7] == 0x05 && c[15] == 0xC1, "NCA: счётчик CTR (ctr_upper || BE(off>>4))");

    const auto key = nca::decrypt_key_area_entry(h, nca::kKeyAreaCtr, s.kaek);
    CHECK(std::memcmp(key.data(), s.section_key, 16) == 0, "NCA: key area [2] расшифрована KAEK");

    int calls = 0;
    nca::SectionReader r(reader(s.body, &calls), nca::soft_ctr(key.data()), sec);
    nca::ExefsReader ex(r);
    CHECK(ex.pfs0().entries.size() == 3, "PFS0: три файла");
    bool all = true;
    for (const auto& f : s.files) {
        const nca::Pfs0Entry* e = ex.pfs0().find(f.first);
        if (!e || e->size != f.second.size()) { all = false; continue; }
        Bytes got(e->size);
        ex.read(*e, 0, got.data(), got.size());
        if (got != f.second) all = false;
    }
    CHECK(all, "exefs: все файлы побайтно равны исходным");
    // невыровненные чтения через границу блока и куска 256 КБ не нужны — файл меньше; проверим смещения
    const nca::Pfs0Entry* m = ex.pfs0().find("main");
    bool okOdd = m != nullptr;
    if (m) {
        for (uint64_t off : {uint64_t(1), uint64_t(15), uint64_t(17), uint64_t(0x1000 - 3), uint64_t(0x12340)}) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(37, m->size - off));
            Bytes got(n);
            ex.read(*m, off, got.data(), n);
            if (std::memcmp(got.data(), s.files["main"].data() + off, n) != 0) okOdd = false;
        }
    }
    CHECK(okOdd, "exefs: чтения с невыровненных смещений");
    CHECK(throws([&] { Bytes b(2); ex.read(*m, m->size - 1, b.data(), 2); }, "NCA_RANGE"), "негатив: чтение за концом файла");

    // негатив: не тот ключ секции — PFS0 не узнаётся
    uint8_t bad[16];
    std::memcpy(bad, key.data(), 16);
    bad[0] ^= 1;
    nca::SectionReader rb(reader(s.body), nca::soft_ctr(bad), sec);
    CHECK(throws([&] { nca::ExefsReader x(rb); }, "NCA_PFS0_MAGIC"), "негатив: не тот ключ -> NCA_PFS0_MAGIC");
    // негатив: не тот KAEK
    uint8_t kaek2[16];
    std::memcpy(kaek2, s.kaek, 16);
    kaek2[5] ^= 0x80;
    const auto key2 = nca::decrypt_key_area_entry(h, nca::kKeyAreaCtr, kaek2);
    CHECK(std::memcmp(key2.data(), s.section_key, 16) != 0, "негатив: не тот KAEK даёт другой ключ");
}

static void test_nca_negative() {
    Synth s = make_nca();
    Bytes h = s.header;
    h[0x200] = 'X';
    CHECK(throws([&] { nca::parse_header(h.data(), h.size(), soft_hash()); }, "NCA_HEADER"), "негатив: нет магии NCA3");
    CHECK(throws([&] { nca::parse_header(s.header.data(), 0x400, soft_hash()); }, "NCA_HEADER"), "негатив: короткий заголовок");
    h = s.header;
    h[0x400 + 0x10] ^= 1;   // правка заголовка секции без пересчёта хеша
    CHECK(throws([&] { nca::parse_header(h.data(), h.size(), soft_hash()); }, "NCA_FS_HASH"), "негатив: хеш заголовка секции");

    Synth x = make_nca(nca::kEncCtrEx);
    const nca::Header hx = nca::parse_header(x.header.data(), x.header.size(), soft_hash());
    CHECK(throws([&] { nca::exefs_section(hx); }, "NCA_EXEFS_CRYPTO"), "негатив: шифрование BKTR (CtrEx) не принимается");

    h = s.header;
    h[0x205] = 2;   // Control
    nca::Header hc = nca::parse_header(h.data(), h.size(), soft_hash());
    CHECK(throws([&] { nca::exefs_section(hc); }, "NCA_NOT_PROGRAM"), "негатив: не программный NCA");

    Synth r = make_nca(nca::kEncCtr, true);
    const nca::Header hr = nca::parse_header(r.header.data(), r.header.size(), soft_hash());
    CHECK(hr.has_rights_id && nca::rights_id_hex(hr.rights_id) == "01000000000000000000000000000013", "NCA: rights id @0x230");

    // PFS0: имя за таблицей строк, файл за данными
    Bytes p = make_pfs0({{"main", Bytes(16, 1)}});
    Bytes p1 = p;
    wr32(p1.data() + 0x10 + 0x10, 0x1000);
    CHECK(throws([&] { nca::parse_pfs0(p1.data(), p1.size(), 16); }, "NCA_PFS0_FORMAT"), "негатив: имя PFS0 за таблицей строк");
    CHECK(throws([&] { nca::parse_pfs0(p.data(), p.size(), 8); }, "NCA_PFS0_FORMAT"), "негатив: файл PFS0 за границей данных");
    Bytes p2 = p;
    p2[0x10 + 0x18] = '/';
    CHECK(throws([&] { nca::parse_pfs0(p2.data(), p2.size(), 16); }, "NCA_PFS0_FORMAT"), "негатив: имя с '/'");
    // за концом секции
    const nca::Header hs = nca::parse_header(s.header.data(), s.header.size(), soft_hash());
    nca::SectionReader rr(reader(s.body), nca::soft_ctr(s.section_key), nca::exefs_section(hs));
    CHECK(throws([&] { uint8_t b[4]; rr.read(hs.sections[0].size - 2, b, 4); }, "NCA_RANGE"), "негатив: чтение за концом секции");
}

static void test_ticket() {
    Bytes t(4 + 0x100 + 0x3C + 0x180 + 0x40, 0);
    wr32(t.data(), 0x10004);
    uint8_t* c = t.data() + 4 + 0x100 + 0x3C;
    for (int i = 0; i < 16; ++i) c[0x40 + i] = uint8_t(0xC0 + i);
    c[0x141] = 0;
    c[0x145] = 0x13;
    c[0x160] = 0x01;
    c[0x16F] = 0x13;
    const nca::Ticket k = nca::parse_ticket(t.data(), t.size());
    CHECK(k.titlekey_type == 0 && k.key_generation == 0x13, "билет: тип ключа и поколение");
    CHECK(k.enc_titlekey[0] == 0xC0 && k.enc_titlekey[15] == 0xCF, "билет: зашифрованный титульный ключ");
    CHECK(nca::rights_id_hex(k.rights_id) == "01000000000000000000000000000013", "билет: rights id");
    Bytes bad = t;
    wr32(bad.data(), 0x12345);
    CHECK(throws([&] { nca::parse_ticket(bad.data(), bad.size()); }, "TICKET_FORMAT"), "негатив: неизвестная подпись билета");
    CHECK(throws([&] { nca::parse_ticket(t.data(), 0x200); }, "TICKET_FORMAT"), "негатив: обрезанный билет");
}

int main() {
    test_aes_vectors();
    test_nca_exefs();
    test_nca_negative();
    test_ticket();
    std::printf("nca: прошло %d, провалено %d\n", g_pass, g_fail);
    if (g_pass + g_fail < 25) {
        std::printf("ЗАТВОР НУЛЯ: проверок меньше 25\n");
        return 2;
    }
    return g_fail ? 1 : 0;
}
