// ModKitNX — хост-тесты построителя форвардера (mk::forwarder): TitleID, NPDM, NACP, три NCA.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NCA собираются с выдуманным ключом заголовка (байты 0x40..0x5F), расшифровываются программным XTS ядра и
// разбираются независимым путём: заголовок — mk::nca::parse_header (магия, хеши заголовков секций), PFS0 —
// mk::nca::parse_pfs0, romfs и слои IVFC — разбором здесь же. Настоящих ключей и файлов консоли нет.
// Необязательно:
//   MK_FWD_DUMP=<папка>     — записать program/control/meta.nca и key.bin (выдуманный ключ) для внешней сверки
//                              (скрипт на Python с другой реализацией AES-XTS и SHA-256);
//   MK_FWD_NPDM=<файл>      — main.npdm, собранный npdmtool из hbl/hbl.json: правка находит флаги ядра и отладки;
//   MK_FWD_AUTORUN=<папка>  — nca-1.bin, nca-2.bin, nca-3.bin, собранные forwarder.c (порт sphaira, проверен на
//                              консоли) с открытым заголовком: пересобрать из их же частей и сравнить побайтно.
#include "mk/aes128.hpp"
#include "mk/core.hpp"
#include "mk/forwarder.hpp"
#include "mk/nca.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

using namespace mk;
namespace fw = mk::forwarder;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, what)                                                                 \
    do {                                                                                  \
        if (cond) { g_pass++; }                                                           \
        else { g_fail++; std::printf("  НЕТ %s (%s:%d)\n", what, __FILE__, __LINE__); } \
    } while (0)

template <class F> static bool throws(F f, const char* code) {
    try {
        f();
    } catch (const Error& e) {
        return e.code() == code;
    } catch (...) {
        return false;
    }
    return false;
}

static Bytes from_hex(const char* h) {
    Bytes b;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) b.push_back(uint8_t(std::stoi(std::string(h + i, 2), nullptr, 16)));
    return b;
}
static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
static uint64_t le64(const uint8_t* p) { return le32(p) | uint64_t(le32(p + 4)) << 32; }
static void setle32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

static uint8_t g_key[32];
static void fake_key() { for (int i = 0; i < 32; ++i) g_key[i] = uint8_t(0x40 + i); }
static void crypt_soft(uint8_t* h, size_t n) { aes::xts_nintendo(g_key, 0, 0x200, h, n, true); }

// ------------------------------------------------------------ AES-XTS
static void test_xts() {
    // IEEE P1619 / XTS-AES-128, вектор 1: ключи нули, сектор 0, 32 нулевых байта (сектор 0 одинаков при любом
    // порядке байт номера сектора).
    uint8_t key[32] = {};
    Bytes d(32, 0);
    aes::xts_nintendo(key, 0, 0x200, d.data(), d.size(), true);
    CHECK(d == from_hex("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e"), "XTS: IEEE P1619 вектор 1");
    // Шесть секторов заголовка туда и обратно; сектор 1 не равен сектору 0 при одинаковых данных (tweak работает).
    fake_key();
    Bytes h(0xC00, 0x5A), e = h;
    aes::xts_nintendo(g_key, 0, 0x200, e.data(), e.size(), true);
    CHECK(std::memcmp(e.data(), e.data() + 0x200, 0x200) != 0, "XTS: разные секторы — разный шифротекст");
    aes::xts_nintendo(g_key, 0, 0x200, e.data(), e.size(), false);
    CHECK(e == h, "XTS: расшифровка возвращает исходное");
}

// ------------------------------------------------------------ TitleID и фильтр списка игр
static void test_title_id() {
    IHash& H = soft_hash();
    const std::string p = "/switch/ModKitNX/ModKitNX.nro";
    CHECK(fw::next_argv(p, "") == p, "argv: без аргументов — сам путь");
    CHECK(fw::next_argv(p, "-x") == p + " -x", "argv: путь + пробел + аргументы");
    // Значения посчитаны независимо (Python hashlib: sha256(путь+путь), первые 8 байт LE & 0x00FFFFFFFFFFF000 | 0x05…).
    CHECK(fw::title_id(p, "", H) == 0x05842FF9E1B75000ULL, "TitleID нашего .nro (вид пути sphaira) = 05842FF9E1B75000");
    CHECK(fw::title_id("sdmc:" + p, "", H) == 0x0565DE3B7FE27000ULL, "TitleID при пути с sdmc: — другой (0565DE3B7FE27000)");
    CHECK((fw::title_id(p, "", H) & 0xFFF) == 0, "TitleID: младшие 12 бит — нули");
    CHECK(fw::is_forwarder_id(0x05842FF9E1B75000ULL), "05… — форвардер");
    CHECK(!fw::is_forwarder_id(0x0100A6B01F2D4000ULL), "01… — игра");
    CHECK(!fw::is_forwarder_id(0x0700000000001000ULL), "07… — не форвардер (строже маски sphaira)");
    CHECK(fw::hidden_from_games(0x0104444444444000ULL), "ярлык hbmenu скрыт");
    CHECK(fw::hidden_from_games(0x0500000000001000ULL), "любой 05… скрыт");
    CHECK(!fw::hidden_from_games(0x0100A6B01F2D4000ULL), "игра не скрыта");
    CHECK(fw::cnmt_name(0x05842FF9E1B75000ULL) == "Application_05842FF9E1B75000.cnmt", "имя cnmt — заглавные hex");
}

// ------------------------------------------------------------ NPDM
// Синтетический main.npdm: META 0x80, ACI0 0x40 + KAC, ACID 0x240 + KAC. В KAC — флаги ядра (шаблон 0b111), флаги
// отладки (шаблон 16 единиц) и посторонняя запись.
static Bytes fake_npdm() {
    const uint32_t aci0 = 0x80, aciKacOff = 0x40, kacSize = 12;
    const uint32_t acid = aci0 + aciKacOff + kacSize + 4, acidKacOff = 0x240;
    Bytes n(acid + acidKacOff + kacSize, 0);
    std::memcpy(n.data(), "META", 4);
    n[0xC] = 0x01 | (3 << 1);   // 64 бит, пространство 39 бит — патч должен сменить на 36
    std::memcpy(n.data() + 0x20, "hbloader", 8);
    std::memcpy(n.data() + 0x30, "PRODUCT", 7);
    setle32(n.data() + 0x70, aci0);
    setle32(n.data() + 0x78, acid);
    std::memcpy(n.data() + aci0, "ACI0", 4);
    setle32(n.data() + aci0 + 0x30, aciKacOff);
    setle32(n.data() + aci0 + 0x34, kacSize);
    std::memcpy(n.data() + acid + 0x200, "ACID", 4);
    setle32(n.data() + acid + 0x230, acidKacOff);
    setle32(n.data() + acid + 0x234, kacSize);
    for (uint32_t base : {aci0 + aciKacOff, acid + acidKacOff}) {
        setle32(n.data() + base + 0, 0x0000002F);          // посторонний описатель (handle table…) — не трогать
        setle32(n.data() + base + 4, 0x030073F7);          // флаги ядра: 4 ядра, 63
        setle32(n.data() + base + 8, (1u << 17) | 0xFFFF); // флаги отладки: ForceDebugProd (BIT 17) + шаблон
    }
    return n;
}

static void test_npdm() {
    CHECK((fw::kernel_flags_three_cores() | 0x7) == 0x020073B7, "флаги ядра: как static_assert sphaira (0x020073B7)");
    const uint64_t tid = 0x05842FF9E1B75000ULL;
    for (bool dbg : {false, true}) {
        Bytes n = fake_npdm();
        fw::patch_npdm(n, tid, dbg);
        const uint32_t aci0 = le32(n.data() + 0x70), acid = le32(n.data() + 0x78);
        CHECK(std::memcmp(n.data() + 0x20, "Application\0\0\0\0\0", 16) == 0, "npdm: имя Application");
        CHECK(std::all_of(n.begin() + 0x30, n.begin() + 0x40, [](uint8_t b) { return b == 0; }), "npdm: product_code пуст");
        CHECK(n[0xC] == (0x01 | (1 << 1)), "npdm: 64 бит сохранён, пространство 36 бит");
        CHECK(le64(n.data() + aci0 + 0x10) == tid, "npdm: ACI0 program_id");
        CHECK(le64(n.data() + acid + 0x210) == tid && le64(n.data() + acid + 0x218) == tid, "npdm: ACID min/max");
        for (uint32_t base : {aci0 + 0x40, acid + 0x240}) {
            CHECK(le32(n.data() + base) == 0x2F, "npdm: посторонний описатель не тронут");
            CHECK(le32(n.data() + base + 4) == 0x020073B7, "npdm: флаги ядра — три ядра");
            const uint32_t want = dbg ? ((1u << 19) | 0xFFFF) : ((1u << 17) | 0xFFFF);
            CHECK(le32(n.data() + base + 8) == want, dbg ? "npdm: ForceDebug (BIT 19) вместо прежнего бита"
                                                          : "npdm: без force_debug флаги отладки не тронуты");
        }
    }
    Bytes junk(0x40, 0);
    CHECK(throws([&] { fw::patch_npdm(junk, tid, false); }, "NPDM_BAD"), "npdm: не NPDM — NPDM_BAD");
    Bytes noKac = fake_npdm();
    const uint32_t aci0 = le32(noKac.data() + 0x70);
    setle32(noKac.data() + aci0 + 0x40 + 4, 0x0000003F);
    CHECK(throws([&] { fw::patch_npdm(noKac, tid, false); }, "NPDM_BAD"), "npdm: нет флагов ядра — NPDM_BAD");
}

// ------------------------------------------------------------ NACP
static void test_nacp() {
    namespace o = fw::nacp_off;
    Bytes n = fw::default_nacp();
    CHECK(n.size() == 0x4000 && n[o::kRatingAge] == 0xFF && n[o::kRatingAge + 0x1F] == 0xFF, "nacp: по умолчанию «не оценено»");
    std::memcpy(n.data() + 0x3000, "ISBN-KEEP", 9);       // поле, которое правка не трогает
    n[o::kStartupUserAccount] = 1;
    setle32(n.data() + o::kSupportedLanguageFlag, 0xBFF);
    n[o::kTitlesDataFormat] = 1;
    for (size_t off : {o::kUserAccountSaveDataSize, o::kDeviceSaveDataSizeMax}) n[off] = 0x77;
    fw::NacpPatch p;
    p.name = "ModKitNX";
    p.author = "qret";
    p.version = "1.1.1";
    p.error_code_category = "ModKitNX";
    p.tid = 0x05842FF9E1B75000ULL;
    fw::patch_nacp(n, p);
    bool names = true;
    for (size_t i = 0; i < 16; ++i)
        names = names && std::strcmp(reinterpret_cast<const char*>(n.data() + i * 0x300), "ModKitNX") == 0 &&
                std::strcmp(reinterpret_cast<const char*>(n.data() + i * 0x300 + 0x200), "qret") == 0;
    CHECK(names, "nacp: имя и автор во всех 16 языках");
    CHECK(std::strcmp(reinterpret_cast<const char*>(n.data() + o::kDisplayVersion), "1.1.1") == 0, "nacp: версия");
    CHECK(std::memcmp(n.data() + 0x3000, "ISBN-KEEP", 9) == 0, "nacp: прочие поля сохранены");
    CHECK(le32(n.data() + o::kSupportedLanguageFlag) == 1, "nacp: поддержан только American English");
    CHECK(n[o::kTitlesDataFormat] == 0 && n[o::kStartupUserAccount] == 0, "nacp: таблица несжатая, без выбора профиля");
    CHECK(n[o::kAocRegistrationType] == 1 && n[o::kVideoCapture] == 2 && n[o::kLogoType] == 2, "nacp: как sphaira");
    CHECK(std::memcmp(n.data() + o::kErrorCodeCategory, "ModKitNX", 8) == 0, "nacp: категория кода ошибки (8 знаков)");
    CHECK(le64(n.data() + o::kPresenceGroupId) == p.tid && le64(n.data() + o::kSaveDataOwnerId) == p.tid &&
              le64(n.data() + o::kPseudoDeviceIdSeed) == p.tid && le64(n.data() + o::kAocBaseId) == (p.tid ^ 0x1000),
          "nacp: TitleID в presence/save owner/seed/aoc");
    bool lc = true;
    for (size_t i = 0; i < 8; ++i) lc = lc && le64(n.data() + o::kLocalCommunicationId + i * 8) == p.tid;
    CHECK(lc, "nacp: local_communication_id × 8");
    CHECK(le64(n.data() + o::kUserAccountSaveDataSize) == 0 && le64(n.data() + o::kDeviceSaveDataSizeMax) == 0,
          "nacp: сохранений нет");
    fw::NacpPatch longName = p;
    longName.name = std::string(0x300, 'A');
    fw::patch_nacp(n, longName);
    CHECK(n[0x1FF] == 0 && n[0x1FE] == 'A', "nacp: длинное имя обрезано с нулём в конце поля");
    Bytes small(0x100, 0);
    CHECK(throws([&] { fw::patch_nacp(small, p); }, "NACP_BAD"), "nacp: не 0x4000 — NACP_BAD");
}

// ------------------------------------------------------------ разбор romfs (независимо от построителя)
static bool romfs_find(const Bytes& r, const std::string& name, Bytes& out) {
    if (r.size() < 0x50 || le64(r.data()) != 0x50) return false;
    const uint64_t fileTableOff = le64(r.data() + 0x38), fileTableSize = le64(r.data() + 0x40), dataOff = le64(r.data() + 0x48);
    const uint64_t dirTableOff = le64(r.data() + 0x18);
    uint32_t off = le32(r.data() + dirTableOff + 0x0C);   // первый файл корня
    while (off != 0xFFFFFFFFu && off + 0x20 <= fileTableSize) {
        const uint8_t* e = r.data() + fileTableOff + off;
        const uint32_t len = le32(e + 0x1C);
        if (std::string(reinterpret_cast<const char*>(e + 0x20), len) == name) {
            const uint64_t at = dataOff + le64(e + 8), size = le64(e + 0x10);
            if (at + size > r.size()) return false;
            out.assign(r.begin() + at, r.begin() + at + size);
            return true;
        }
        off = le32(e + 4);
    }
    return false;
}

static Bytes dec_header(const Bytes& nca) {
    Bytes h(nca.begin(), nca.begin() + 0xC00);
    aes::xts_nintendo(g_key, 0, 0x200, h.data(), h.size(), false);
    return h;
}

// Секция romfs: уровни IVFC сходятся снизу вверх и к мастер-хешу; отдаёт образ romfs (уровень 5).
static bool check_ivfc(const Bytes& nca, const Bytes& hdr, int index, Bytes& romfs, const char* what) {
    IHash& H = soft_hash();
    const uint8_t* fs = hdr.data() + 0x400 + index * 0x200;
    const uint64_t start = uint64_t(le32(hdr.data() + 0x240 + index * 0x10)) * 0x200;
    bool ok = std::memcmp(fs + 0x8, "IVFC", 4) == 0 && le32(fs + 0x14) == 7;
    Bytes lv[6];
    for (int i = 0; i < 6 && ok; ++i) {
        const uint8_t* l = fs + 0x18 + i * 0x18;
        const uint64_t off = le64(l), size = le64(l + 8);
        ok = ok && le32(l + 0x10) == 0x0E && start + off + size <= nca.size();
        if (ok) lv[i].assign(nca.begin() + start + off, nca.begin() + start + off + size);
    }
    const Digest m = H.sha256(lv[0].data(), lv[0].size());
    ok = ok && std::memcmp(m.data(), fs + 0xC8, 32) == 0;
    for (int i = 1; i < 6 && ok; ++i) {
        for (size_t b = 0, k = 0; b < lv[i].size() && ok; b += 0x4000, ++k) {
            const size_t n = std::min<size_t>(0x4000, lv[i].size() - b);
            Bytes blk(lv[i].begin() + b, lv[i].begin() + b + n);
            if (i == 5) blk.resize(0x4000, 0);   // последний блок romfs хешируется с выравниванием
            const Digest d = H.sha256(blk.data(), blk.size());
            ok = ok && (k + 1) * 32 <= lv[i - 1].size() && std::memcmp(d.data(), lv[i - 1].data() + k * 32, 32) == 0;
        }
    }
    CHECK(ok, what);
    romfs = lv[5];
    return ok;
}

static bool check_pfs0(const Bytes& nca, const nca::FsSection& s, const Bytes& hdr, int index, nca::Pfs0& pfs,
                       Bytes& body, const char* what) {
    IHash& H = soft_hash();
    const uint8_t* fs = hdr.data() + 0x400 + index * 0x200;
    const uint32_t block = le32(fs + 0x28);
    const uint64_t tableSize = le64(fs + 0x38);
    bool ok = s.fs_type == nca::kFsPartition && s.hash_type == nca::kHashSha256 && s.encryption_type == nca::kEncNone;
    ok = ok && s.offset + s.pfs0_offset + s.pfs0_size <= nca.size();
    if (!ok) { CHECK(false, what); return false; }
    const Bytes table(nca.begin() + s.offset, nca.begin() + s.offset + tableSize);
    const Digest m = H.sha256(table.data(), table.size());
    ok = std::memcmp(m.data(), fs + 0x8, 32) == 0;
    body.assign(nca.begin() + s.offset + s.pfs0_offset, nca.begin() + s.offset + s.pfs0_offset + s.pfs0_size);
    for (size_t b = 0, k = 0; b < body.size() && ok; b += block, ++k) {
        const Digest d = H.sha256(body.data() + b, std::min<size_t>(block, body.size() - b));
        ok = (k + 1) * 32 <= table.size() && std::memcmp(d.data(), table.data() + k * 32, 32) == 0;
    }
    if (ok) {
        const uint64_t hs = nca::pfs0_header_size(body.data());
        pfs = nca::parse_pfs0(body.data(), hs, body.size() - hs);
    }
    CHECK(ok, what);
    return ok;
}

static Bytes file_of(const Bytes& body, const nca::Pfs0& p, const std::string& name) {
    const nca::Pfs0Entry* e = p.find(name);
    if (!e) return {};
    return Bytes(body.begin() + p.header_size + e->offset, body.begin() + p.header_size + e->offset + e->size);
}

static fw::Input sample_input() {
    fw::Input in;
    in.tid = 0x05842FF9E1B75000ULL;
    in.hbl_main.resize(0x1234B);
    for (size_t i = 0; i < in.hbl_main.size(); ++i) in.hbl_main[i] = uint8_t(i * 7 + 3);
    in.npdm = fake_npdm();
    fw::patch_npdm(in.npdm, in.tid, true);
    in.nacp = fw::default_nacp();
    fw::NacpPatch p;
    p.name = "ModKitNX";
    p.author = "qret";
    p.version = "1.1.1";
    p.error_code_category = "ModKitNX";
    p.tid = in.tid;
    fw::patch_nacp(in.nacp, p);
    in.icon.assign(0x5123, 0xAB);
    in.icon[0] = 0xFF;
    in.icon[1] = 0xD8;
    in.nro_path = "/switch/ModKitNX/ModKitNX.nro";
    in.argv = fw::next_argv(in.nro_path, "");
    return in;
}

static void test_build() {
    IHash& H = soft_hash();
    fake_key();
    const fw::Input in = sample_input();
    const fw::Output o = fw::build(in, H, crypt_soft);
    const fw::Output again = fw::build(in, H, crypt_soft);
    CHECK(o.program.data == again.program.data && o.meta.data == again.meta.data, "сборка детерминирована");

    struct Part { const fw::Nca* n; uint8_t ncaType; const char* name; };
    for (const Part& part : {Part{&o.program, 0, "program"}, Part{&o.control, 2, "control"}, Part{&o.meta, 1, "meta"}}) {
        const Bytes& d = part.n->data;
        const Digest h = H.sha256(d.data(), d.size());
        CHECK(h == part.n->hash, (std::string(part.name) + ": хеш NCA").c_str());
        CHECK(d.size() % 0x200 == 0, (std::string(part.name) + ": размер кратен 0x200").c_str());
        CHECK(std::memcmp(d.data() + 0x200, "NCA3", 4) != 0, (std::string(part.name) + ": заголовок зашифрован").c_str());
        const Bytes hdr = dec_header(d);
        nca::Header nh;
        bool parsed = true;
        try {
            nh = nca::parse_header(hdr.data(), hdr.size(), H);   // магия + хеши заголовков секций
        } catch (const Error& e) {
            parsed = false;
            std::printf("    %s: %s\n", part.name, e.what());
        }
        CHECK(parsed, (std::string(part.name) + ": заголовок разбирается, хеши секций сходятся").c_str());
        if (!parsed) continue;
        CHECK(nh.content_type == part.ncaType && nh.program_id == in.tid && nh.content_size == d.size() &&
                  !nh.has_rights_id && nh.key_generation == 0,
              (std::string(part.name) + ": тип, TitleID, размер, без rights id").c_str());
        CHECK(hdr[0x204] == 0 && le32(hdr.data() + 0x21C) == 0x000C1100, (std::string(part.name) + ": System, sdk 0x000C1100").c_str());
        CHECK(nh.sections[0].present && nh.sections[0].offset == 0xC00, (std::string(part.name) + ": секция 0 с 0xC00").c_str());

        if (part.ncaType == 0) {
            nca::Pfs0 pfs;
            Bytes body, romfs, got;
            if (check_pfs0(d, nh.sections[0], hdr, 0, pfs, body, "program: exefs PFS0 и хеши (блок 0x10000)")) {
                CHECK(pfs.entries.size() == 2 && pfs.entries[0].name == "main" && pfs.entries[1].name == "main.npdm",
                      "program: exefs = main, main.npdm");
                CHECK(file_of(body, pfs, "main") == in.hbl_main && file_of(body, pfs, "main.npdm") == in.npdm,
                      "program: main и main.npdm побайтно");
            }
            CHECK(nh.sections[1].present && nh.sections[1].offset == nh.sections[0].offset + nh.sections[0].size,
                  "program: romfs сразу за exefs");
            if (check_ivfc(d, hdr, 1, romfs, "program: IVFC romfs сходится к мастер-хешу")) {
                CHECK(romfs_find(romfs, "nextNroPath", got) && std::string(got.begin(), got.end()) == in.nro_path,
                      "program: /nextNroPath");
                CHECK(romfs_find(romfs, "nextArgv", got) && std::string(got.begin(), got.end()) == in.argv,
                      "program: /nextArgv");
            }
            CHECK(!nh.sections[2].present && !nh.sections[3].present, "program: секций 2 и 3 нет");
        } else if (part.ncaType == 2) {
            Bytes romfs, got;
            if (check_ivfc(d, hdr, 0, romfs, "control: IVFC romfs сходится к мастер-хешу")) {
                CHECK(romfs_find(romfs, "control.nacp", got) && got == in.nacp, "control: control.nacp");
                CHECK(romfs_find(romfs, "icon_AmericanEnglish.dat", got) && got == in.icon, "control: иконка");
            }
            CHECK(!nh.sections[1].present, "control: одна секция");
        } else {
            nca::Pfs0 pfs;
            Bytes body;
            if (check_pfs0(d, nh.sections[0], hdr, 0, pfs, body, "meta: PFS0 и хеши (блок 0x1000)")) {
                const Bytes cnmt = file_of(body, pfs, fw::cnmt_name(in.tid));
                CHECK(pfs.entries.size() == 1 && cnmt == o.cnmt && cnmt.size() == 0xC0, "meta: один файл — cnmt 0xC0");
                if (cnmt.size() == 0xC0) {
                    CHECK(le64(cnmt.data()) == in.tid && cnmt[0x0C] == 0x80 && le32(cnmt.data() + 8) == 0,
                          "cnmt: TitleID, Application, версия 0");
                    CHECK(cnmt[0x0E] == 0x10 && cnmt[0x10] == 2 && cnmt[0x12] == 1 && cnmt[0x15] == 5,
                          "cnmt: ext 0x10, 2 содержимых, хранилище SD");
                    CHECK(le64(cnmt.data() + 0x20) == (in.tid | 0x800), "cnmt: patch_id");
                    const fw::Nca* two[2] = {&o.program, &o.control};
                    const uint8_t types[2] = {1, 3};
                    for (int i = 0; i < 2; ++i) {
                        const uint8_t* e = cnmt.data() + 0x30 + i * 0x38;
                        CHECK(std::memcmp(e, two[i]->hash.data(), 32) == 0 && std::memcmp(e + 0x20, two[i]->hash.data(), 16) == 0 &&
                                  le32(e + 0x30) == two[i]->data.size() && e[0x36] == types[i],
                              "cnmt: хеш, content id, размер, тип содержимого");
                    }
                }
            }
        }
    }
    // Запись базы ncm.
    const Bytes& r = o.meta_db_record;
    CHECK(r.size() == 0x60, "запись ncm: 0x60 байт");
    if (r.size() == 0x60) {
        CHECK(r[0] == 0x10 && r[2] == 3 && r[4] == 1 && r[7] == 0, "запись ncm: заголовок (3 содержимых, хранилище 0)");
        CHECK(le64(r.data() + 8) == (in.tid | 0x800), "запись ncm: patch_id");
        CHECK(std::memcmp(r.data() + 0x18, o.meta.hash.data(), 16) == 0 && r[0x18 + 0x16] == 0 &&
                  le32(r.data() + 0x18 + 0x10) == o.cnmt.size(),
              "запись ncm: meta (размер — cnmt, как sphaira)");
        CHECK(std::memcmp(r.data() + 0x30, o.program.hash.data(), 16) == 0 && r[0x30 + 0x16] == 1, "запись ncm: program");
        CHECK(std::memcmp(r.data() + 0x48, o.control.hash.data(), 16) == 0 && r[0x48 + 0x16] == 3, "запись ncm: control");
    }
    // Неполные входы.
    fw::Input bad = in;
    bad.icon.assign(0x20001, 0);
    CHECK(throws([&] { fw::build(bad, H, crypt_soft); }, "FWD_BAD_INPUT"), "иконка > 0x20000 — FWD_BAD_INPUT");
    bad = in;
    bad.nacp.resize(0x100);
    CHECK(throws([&] { fw::build(bad, H, crypt_soft); }, "FWD_BAD_INPUT"), "NACP не 0x4000 — FWD_BAD_INPUT");
    bad = in;
    bad.hbl_main.clear();
    CHECK(throws([&] { fw::build(bad, H, crypt_soft); }, "FWD_BAD_INPUT"), "нет загрузчика — FWD_BAD_INPUT");

    // Выгрузка для внешней сверки.
    if (const char* dir = std::getenv("MK_FWD_DUMP")) {
        auto put = [dir](const char* n, const Bytes& b) {
            std::ofstream f(std::string(dir) + "/" + n, std::ios::binary);
            f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        };
        put("program.nca", o.program.data);
        put("control.nca", o.control.data);
        put("meta.nca", o.meta.data);
        put("key.bin", Bytes(g_key, g_key + 32));
        put("meta_db_record.bin", o.meta_db_record);
        std::printf("  выгружено в %s\n", dir);
    }
}

// ------------------------------------------------------------ сверка с forwarder.c (по желанию)
static bool read_file(const std::string& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static void test_autorun_reference() {
    const char* dir = std::getenv("MK_FWD_AUTORUN");
    if (!dir) {
        std::printf("  сверка с forwarder.c пропущена (MK_FWD_AUTORUN не задан)\n");
        return;
    }
    IHash& H = soft_hash();
    Bytes ref[3];
    for (int i = 0; i < 3; ++i)
        if (!read_file(std::string(dir) + "/nca-" + std::to_string(i + 1) + ".bin", ref[i])) {
            CHECK(false, "forwarder.c: nca-1..3.bin читаются");
            return;
        }
    // Заголовки открыты (шим forwarder.c не шифрует): части берутся из его же NCA.
    auto plain = [](uint8_t*, size_t) {};
    nca::Header hp = nca::parse_header(ref[0].data(), 0xC00, H), hc = nca::parse_header(ref[1].data(), 0xC00, H);
    fw::Input in;
    in.tid = hp.program_id;
    nca::Pfs0 pfs;
    Bytes body, romfs, nacp, icon, argv, path;
    const Bytes rh0(ref[0].begin(), ref[0].begin() + 0xC00), rh1(ref[1].begin(), ref[1].begin() + 0xC00);
    check_pfs0(ref[0], hp.sections[0], rh0, 0, pfs, body, "forwarder.c: exefs его program NCA сходится");
    in.hbl_main = file_of(body, pfs, "main");
    in.npdm = file_of(body, pfs, "main.npdm");
    check_ivfc(ref[0], rh0, 1, romfs, "forwarder.c: romfs его program NCA сходится");
    romfs_find(romfs, "nextArgv", argv);
    romfs_find(romfs, "nextNroPath", path);
    in.argv.assign(argv.begin(), argv.end());
    in.nro_path.assign(path.begin(), path.end());
    check_ivfc(ref[1], rh1, 0, romfs, "forwarder.c: romfs его control NCA сходится");
    romfs_find(romfs, "control.nacp", in.nacp);
    romfs_find(romfs, "icon_AmericanEnglish.dat", in.icon);
    (void)hc;
    const fw::Output o = fw::build(in, H, plain);
    CHECK(o.program.data == ref[0], "forwarder.c: program NCA побайтно");
    CHECK(o.control.data == ref[1], "forwarder.c: control NCA побайтно");
    CHECK(o.meta.data == ref[2], "forwarder.c: meta NCA побайтно");
    std::printf("  сверка с forwarder.c: TitleID %016llX, размеры %zu/%zu/%zu\n",
                static_cast<unsigned long long>(in.tid), ref[0].size(), ref[1].size(), ref[2].size());
}

static void test_real_npdm() {
    const char* path = std::getenv("MK_FWD_NPDM");
    if (!path) {
        std::printf("  настоящий main.npdm не проверялся (MK_FWD_NPDM не задан)\n");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    Bytes n((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint64_t tid = 0x05842FF9E1B75000ULL;
    bool ok = true;
    try {
        fw::patch_npdm(n, tid, true);
    } catch (const Error& e) {
        ok = false;
        std::printf("    %s\n", e.what());
    }
    CHECK(ok, "настоящий main.npdm: правка прошла");
    const uint32_t aci0 = le32(n.data() + 0x70), acid = le32(n.data() + 0x78);
    CHECK(le64(n.data() + aci0 + 0x10) == tid && le64(n.data() + acid + 0x210) == tid, "настоящий main.npdm: TitleID");
    int kernel = 0, debug = 0;
    for (uint32_t base : {aci0 + le32(n.data() + aci0 + 0x30), acid + le32(n.data() + acid + 0x230)}) {
        const uint32_t size = base > acid ? le32(n.data() + acid + 0x234) : le32(n.data() + aci0 + 0x34);
        for (uint32_t i = 0; i + 4 <= size; i += 4) {
            const uint32_t c = le32(n.data() + base + i);
            if (c == 0x020073B7) ++kernel;
            if (c == ((1u << 19) | 0xFFFF)) ++debug;
        }
    }
    CHECK(kernel == 2 && debug == 2, "настоящий main.npdm: флаги ядра и ForceDebug в ACI0 и ACID");
}

int main() {
    test_xts();
    test_title_id();
    test_npdm();
    test_nacp();
    test_build();
    test_real_npdm();
    test_autorun_reference();
    std::printf("forwarder_tests: %d ok, %d fail\n", g_pass, g_fail);
    if (g_pass == 0) return 1;
    return g_fail ? 1 : 0;
}
