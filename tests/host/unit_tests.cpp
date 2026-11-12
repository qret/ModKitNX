// ModKitNX — юнит-тесты ядра на синтетике (без файлов игр). Негативные пробы — обязаны краснеть.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_deflate.h"
#include "host_fs.hpp"
#include "mk/install.hpp"
#include "mk/logs.hpp"
#include "mk/nacp.hpp"
#include "mk/npdm.hpp"
#include "mk/sdk.hpp"
#include "mk/sdk_harvest.hpp"
#include "mk/zbic.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>

using namespace mk;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (cond) { g_pass++; }                                                   \
        else { g_fail++; std::printf("  НЕТ %s (%s:%d)\n", what, __FILE__, __LINE__); } \
    } while (0)

template <class F> static bool throws(F f, const char* code = nullptr) {
    try {
        f();
    } catch (const Error& e) {
        return !code || e.code() == code;
    } catch (...) {
        return !code;
    }
    return false;
}

extern "C" int mk_bic_table_get(int i, int* mid, int* first, int* last);
extern "C" void mk_bic_table_init(void);

static Bytes from_hex(const char* h) {
    Bytes b;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) b.push_back(uint8_t(std::stoi(std::string(h + i, 2), nullptr, 16)));
    return b;
}
static uint32_t crc32(const Bytes& b) {
    uint32_t c = 0xFFFFFFFF;
    for (uint8_t x : b) {
        c ^= x;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// ------------------------------------------------------------ синтетический npdm
static Bytes make_npdm(uint64_t pid, uint64_t fs_mask, std::vector<uint32_t> kern) {
    // ACID: 0x240 шапка + fs (0x2C) + srv (4) + kern; ACI0: 0x40 + fs (0x1C) + srv (4) + kern
    auto sec = [&](bool acid) {
        uint32_t hdr = acid ? 0x240 : 0x40, base = acid ? 0x220 : 0x20;
        Bytes s(hdr, 0);
        if (acid) std::memcpy(&s[0x200], "ACID", 4);
        else { std::memcpy(&s[0], "ACI0", 4); wr64(&s[0x10], pid); }
        if (acid) { wr64(&s[0x210], pid); wr64(&s[0x218], pid); }
        Bytes fs(acid ? 0x2C : 0x1C, 0);
        fs[0] = 1;
        wr64(&fs[4], fs_mask);
        uint32_t fo = uint32_t(s.size());
        s.insert(s.end(), fs.begin(), fs.end());
        while (s.size() % 16) s.push_back(0);
        Bytes srv = {'f', 's', 'p', 0};
        uint32_t so = uint32_t(s.size());
        s.insert(s.end(), srv.begin(), srv.end());
        while (s.size() % 16) s.push_back(0);
        uint32_t ko = uint32_t(s.size());
        for (uint32_t v : kern) { s.resize(s.size() + 4); wr32(&s[s.size() - 4], v); }
        wr32(&s[base], fo); wr32(&s[base + 4], uint32_t(fs.size()));
        wr32(&s[base + 8], so); wr32(&s[base + 12], uint32_t(srv.size()));
        wr32(&s[base + 16], ko); wr32(&s[base + 20], uint32_t(kern.size() * 4));
        if (acid) wr32(&s[0x204], uint32_t(s.size() - 0x100));
        return s;
    };
    Bytes acid = sec(true), aci = sec(false);
    Bytes n(0x80, 0);
    std::memcpy(n.data(), "META", 4);
    uint32_t aci_off = 0x80;
    n.insert(n.end(), aci.begin(), aci.end());
    while (n.size() % 16) n.push_back(0);
    uint32_t acid_off = uint32_t(n.size());
    n.insert(n.end(), acid.begin(), acid.end());
    wr32(&n[0x70], aci_off); wr32(&n[0x74], uint32_t(aci.size()));
    wr32(&n[0x78], acid_off); wr32(&n[0x7C], uint32_t(acid.size()));
    return n;
}

static void test_sha() {
    Bytes abc = {'a', 'b', 'c'};
    CHECK(sha256_hex(soft_hash(), abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(abc)");
    Bytes e;
    CHECK(sha256_hex(soft_hash(), e) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256('')");
    Bytes m(1000000, 'a');
    CHECK(sha256_hex(soft_hash(), m) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256(1e6 'a')");
}

static void test_bic_table() {
    mk_bic_table_init();
    int m, f, l;
    CHECK(mk_bic_table_get(0, &m, &f, &l) && m == 128 && f == 0 && l == 256, "BIC_table[0] = (128,0,256)");
    CHECK(mk_bic_table_get(1, &m, &f, &l) && m == 192 && f == 128 && l == 256, "BIC_table[1] = (192,128,256): правая половина раньше левой");
    CHECK(mk_bic_table_get(255, &m, &f, &l) && m == 0 && f == 0 && l == 1, "BIC_table[255] = (0,0,1) вырожденная");
    CHECK(!mk_bic_table_get(256, &m, &f, &l), "BIC_table: ровно 256 записей");
}

static void test_zstd_frames() {
    // обычный zstd: кадр с одним сырым блоком (single segment, размер 5)
    Bytes fr = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x05, 0x29, 0x00, 0x00, 'H', 'e', 'l', 'l', 'o'};
    Bytes out = zbic_decompress(fr.data(), fr.size(), 5);
    CHECK(std::string(out.begin(), out.end()) == "Hello", "zstd: сырой блок распакован");
    Bytes z = fr;
    std::memcpy(z.data(), "ZBIC", 4);
    out = zbic_decompress(z.data(), z.size(), 5);
    CHECK(std::string(out.begin(), out.end()) == "Hello", "ZBIC: магия ZBIC принята, тот же кадр");
    Bytes two = fr;
    two.insert(two.end(), z.begin(), z.end());
    out = zbic_decompress(two.data(), two.size(), 10);
    CHECK(std::string(out.begin(), out.end()) == "HelloHello", "два кадра подряд (zstd + ZBIC)");
    Bytes bad = fr;
    bad[0] = 0x29;
    CHECK(throws([&] { zbic_decompress(bad.data(), bad.size(), 5); }, "ZBIC_DECODE"), "негатив: чужая магия — отказ");
    CHECK(throws([&] { zbic_decompress(fr.data(), fr.size(), 6); }, "ZBIC_DECODE"), "негатив: размер не сошёлся — отказ");
    Bytes cut(fr.begin(), fr.end() - 2);
    CHECK(throws([&] { zbic_decompress(cut.data(), cut.size(), 5); }), "негатив: обрезанный кадр — отказ");
}

static void test_lz4() {
    Bytes b = {0x50, 'h', 'e', 'l', 'l', 'o'};
    Bytes o = lz4_block_decompress(b.data(), b.size(), 5);
    CHECK(std::string(o.begin(), o.end()) == "hello", "LZ4: литералы");
    // 'a' + повтор 5 (смещение 1) + 12 литералов (LZ4: последний повтор не ближе 12 байт к концу)
    std::string tail12 = "bcdefghijklm";
    Bytes r = {0x11, 'a', 0x01, 0x00, 0xC0};
    r.insert(r.end(), tail12.begin(), tail12.end());
    o = lz4_block_decompress(r.data(), r.size(), 18);
    CHECK(std::string(o.begin(), o.end()) == "aaaaaa" + tail12, "LZ4: повтор с перекрытием");
    Bytes bad = r;
    bad[2] = 0x09;
    CHECK(throws([&] { lz4_block_decompress(bad.data(), bad.size(), 18); }, "NSO_LZ4"), "негатив: LZ4 смещение за начало — отказ");
}

static void test_npdm() {
    uint64_t pid = 0x01007A2027548000ull;
    std::vector<uint32_t> kern = {0x00FFFFEF /*не svc*/, (0u << 29) | (0xFFFFFFu << 5) | 0x0F, (1u << 29) | (0x3u << 5) | 0x0F};
    Bytes g = make_npdm(pid, 0x1, kern);
    Npdm src(g);
    CHECK(src.program_id() == pid, "npdm: program id из ACI0");
    Bytes out = make_game_npdm(g, pid);
    Npdm n(out);
    CHECK((n.fs_mask("ACI0") & kSdCardBit) && (n.fs_mask("ACID") & kSdCardBit), "npdm: бит SdCard в ACI0 и ACID");
    auto a1 = Npdm::syscalls_of(n.caps("ACI0")).first, a2 = Npdm::syscalls_of(n.caps("ACID")).first;
    CHECK(a1.count(0x74) && a1.count(0x75) && a2.count(0x74) && a2.count(0x75), "npdm: svc 0x74/0x75 в обеих секциях");
    CHECK(Npdm::syscall_words(n.caps("ACI0")) == Npdm::syscall_words(n.caps("ACID")), "npdm: маски ACI0 == ACID слово в слово");
    CHECK(npdm_compare(Npdm(npdm_add_sdcard(g)), n, npdm_default_svcs()).empty(), "npdm: кроме масок ничего не изменилось");
    CHECK(rd32(&n.acid[0x204]) == n.acid.size() - 0x100, "npdm: размер ACID (+0x204) пересчитан");
    CHECK(n.caps("ACI0")[0] == 0x00FFFFEF, "npdm: прочие возможности на месте и в том же порядке");
    CHECK(make_game_npdm(g, pid) == out, "npdm: детерминированно (два прогона — одни байты)");
    CHECK(throws([&] { npdm_add_sdcard(out); }, "NPDM_SDCARD"), "негатив: SdCard уже стоит — отказ (ровно 2 байта, как эталон)");
    CHECK(throws([&] { make_game_npdm(g, pid + 1); }, "FAIL_GATE_NPDM"), "негатив: program id не тот — гейт красный");
    Bytes bad = g;
    bad[0] = 'X';
    CHECK(throws([&] { Npdm x(bad); }, "NPDM_BAD"), "негатив: без META — отказ");
    Bytes bad2 = g;
    wr32(&bad2[0x74], 0xFFFF);
    CHECK(throws([&] { Npdm x(bad2); }, "NPDM_BAD"), "негатив: ACI0 за файлом — отказ");
    // npdm мода: уже покрывает наши права -> as-is; иначе merged
    ModNpdm m1 = make_mod_npdm(out, &g, pid);
    CHECK(m1.mode == "as-is" && m1.data == out, "npdm мода с нашими правами — как есть, байт в байт");
    ModNpdm m2 = make_mod_npdm(g, &g, pid);
    CHECK(m2.mode == "merged" && m2.added.size() == 2, "npdm мода без прав — дописано недостающее (SdCard + вызовы)");
    CHECK(throws([&] { make_mod_npdm(g, &g, pid + 2); }, "FAIL_MOD_NPDM_TID"), "негатив: npdm мода от другой программы");
    // ACI0 и ACID разошлись -> check краснеет
    Npdm nn(out);
    Bytes r2 = out;
    uint32_t ko = nn.blocks["ACI0"]["kern"].off;
    r2[nn.aci_off + ko + 4] ^= 0x20;
    CHECK(!npdm_check(Npdm(r2), npdm_default_svcs(), &pid, true).empty(), "негатив: маски ACI0 != ACID — гейт красный");
}

static void test_receipt() {
    ReceiptFields f;
    f.tid = "0100F07026DA8000";
    f.slot = "subsdk9";
    f.module_sha256 = "aa";
    f.npdm_sha256 = "bb";
    f.module_name = "subsdk9";
    f.zbic_files = {"main"};
    f.neighbours = {"z\"q\\x\x01", "ёжик"};
    f.compat = std::vector<std::string>{"ldn", "hash"};
    f.installed_by = "";
    ojson r = make_receipt(f, {});
    r["installed"] = "2026-11-08T21:00:00";
    std::string want =
        "{\n  \"schema\": 1,\n  \"tool\": \"nxredirect\",\n  \"version\": \"1.1\",\n  \"game_tid\": \"0100F07026DA8000\",\n"
        "  \"slot\": \"subsdk9\",\n  \"installed\": \"2026-11-08T21:00:00\",\n  \"module_sha256\": \"aa\",\n"
        "  \"npdm_sha256\": \"bb\",\n  \"module_name\": \"subsdk9\",\n  \"zbic_files\": [\n    \"main\"\n  ],\n"
        "  \"neighbours\": [\n    \"z\\\"q\\\\x\\u0001\",\n    \"ёжик\"\n  ],\n  \"modes\": [\n    \"install\"\n  ],\n"
        "  \"compat\": [\n    \"hash\",\n    \"ldn\"\n  ],\n  \"mod\": null,\n  \"sdk_swap\": null,\n  \"diag\": false,\n"
        "  \"diag_gfx\": false,\n  \"module_variant\": null\n}\n";
    CHECK(dump_receipt(r) == want, "квитанция: текст как json.dumps(indent=2, ensure_ascii=False) эталона");
    if (dump_receipt(r) != want) std::printf("%s\n", dump_receipt(r).c_str());
    // дополнение: прежние zbic_files/modes сохраняются, слот из прежней
    ReceiptRead base = parse_receipt_text(dump_receipt(r));
    ReceiptFields f2;
    f2.tid = f.tid;
    f2.zbic_files = {"sdk"};
    f2.mode = "zbic-only";
    f2.installed_by = "console";
    ojson r2 = make_receipt(f2, base);
    CHECK(r2["slot"] == "subsdk9" && r2["zbic_files"] == ojson({"main", "sdk"}) &&
              r2["modes"] == ojson({"install", "zbic-only"}) && r2["compat"] == ojson({"hash", "ldn"}),
          "квитанция дополняется, а не переписывается (как эталон)");
    std::vector<std::string> keys;
    for (auto& [k, v] : r2.items()) keys.push_back(k);
    CHECK(keys.back() == "module_variant" && keys[keys.size() - 2] == "diag_gfx" && keys[keys.size() - 3] == "diag" &&
              keys[keys.size() - 4] == "sdk_swap" && !r2.contains("installed_by"),
          "квитанция: поля формата 1 до module_variant; installed_by и учёт своего дописывает исполнитель");
    // вид сборки: из прежней квитанции, если прогон его не задаёт (как эталон: --zbic-only не ставит модуль)
    ReceiptFields f3 = f;
    f3.diag = true;
    f3.diag_gfx = true;
    f3.module_variant = std::string("autofix-diag-gfx");
    ReceiptRead b3 = parse_receipt_text(dump_receipt(make_receipt(f3, {})));
    ojson r3 = make_receipt(f2, b3);
    CHECK(r3["diag"] == true && r3["diag_gfx"] == true && r3["module_variant"] == "autofix-diag-gfx",
          "квитанция: diag, diag_gfx, module_variant переносятся из прежней (как эталон)");
    CHECK(module_variant(true, false, false, false) == "autofix" &&
              module_variant(true, true, false, true) == "autofix-diag-earlyfs" &&
              module_variant(true, true, true, false) == "autofix-diag-gfx" &&
              module_variant(false, true, true, false) == "diag-gfx" &&
              module_variant(false, false, true, false) == "plain-gfx" &&
              module_variant(false, false, false, false) == "plain" &&
              module_variant(true, false, false, true) == "autofix-earlyfs",
          "module_variant: имена = id вариантов поставки (module_variant эталона)");
    auto ours = exefs_ours_known(base);
    CHECK(ours.count("subsdk9") && ours.count("main") && ours.count("main.npdm") && ours.count("main.npdm.orig"),
          "exefs_ours_known: слот, zbic, main.npdm(.orig)");
    ReceiptRead broken = parse_receipt_text("{не json");
    CHECK(broken.present && broken.broken && !broken.sane(), "битая квитанция не считается отсутствующей");
    CHECK(parse_receipt_text("[1,2]").broken, "квитанция-массив — битая");
}

static void test_inflate_nacp() {
    Bytes a = from_hex("cb48cdc9c957c8402775147cf353bc334bfc22143125c9520200"), out;
    size_t used = 0;
    std::string want;
    for (int i = 0; i < 3; i++) want += "hello hello hello hello, ModKitNX! ";
    CHECK(inflate_raw(a.data(), a.size(), out, 1 << 20, &used) && std::string(out.begin(), out.end()) == want &&
              used == a.size(), "DEFLATE: фиксированный Хаффман");
    Bytes s = from_hex("010700f8ff73746f72656421");
    CHECK(inflate_raw(s.data(), s.size(), out, 100, &used) && std::string(out.begin(), out.end()) == "stored!",
          "DEFLATE: несжатый блок");
    Bytes d = from_hex(kDeflateDynHex);
    CHECK(inflate_raw(d.data(), d.size(), out, 1 << 20, &used) && out.size() == kDeflateDynLen && crc32(out) == kDeflateDynCrc,
          "DEFLATE: динамический Хаффман (эталон zlib), CRC сошёлся");
    Bytes bad = d;
    bad[bad.size() / 2] ^= 0xFF;
    bool ok = inflate_raw(bad.data(), bad.size(), out, 1 << 20, &used) && crc32(out) == kDeflateDynCrc;
    CHECK(!ok, "негатив: испорченный поток DEFLATE не даёт тех же данных");
    CHECK(!inflate_raw(a.data(), a.size(), out, 10, &used), "негатив: выход больше предела — отказ");

    // NACP: обычная таблица
    Bytes n(0x4000, 0);
    auto put = [&](int lang, const char* name, const char* pub) {
        std::memcpy(&n[lang * 0x300], name, std::strlen(name));
        std::memcpy(&n[lang * 0x300 + 0x200], pub, std::strlen(pub));
    };
    put(11, "Колодезник", "Изд");
    put(1, "Well Dweller", "Pub");
    std::memcpy(&n[0x3060], " 1.0.3 ", 7);
    NacpInfo i = parse_nacp(n.data(), n.size());
    CHECK(i.title == std::string("Well Dweller") && i.title_lang == std::string("BritishEnglish") &&
              i.publisher == std::string("Pub") && i.display_version == std::string("1.0.3") && i.languages.size() == 2 &&
              !i.table_was_compressed,
          "NACP: British, если нет American; версия обрезана");
    // сжатая таблица (SDK 20+): u16 длина + raw DEFLATE — несжатый блок с записями
    Bytes table(n.begin(), n.begin() + 0x3000);
    Bytes z;
    for (size_t off = 0; off < table.size();) {
        size_t len = std::min<size_t>(0xFFFF, table.size() - off);
        bool last = off + len == table.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(len)); z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t(~len)); z.push_back(uint8_t(~len >> 8));
        z.insert(z.end(), table.begin() + off, table.begin() + off + len);
        off += len;
    }
    Bytes nz(0x4000, 0);
    nz[0] = uint8_t(z.size()); nz[1] = uint8_t(z.size() >> 8);
    // 0x3000 + 5 байт заголовка не влезает в таблицу — проверяем отказ (длина > размера)
    CHECK(!unpack_nacp_titles(nz.data()), "сжатая таблица длиннее самой таблицы — не трогается");
    Bytes small(0x600, 0);   // две записи: American + British
    std::memcpy(&small[0], "Игра", std::strlen("Игра"));
    std::memcpy(&small[0x300], "Game", 4);
    Bytes z2 = {1, 0x00, 0x06, 0xFF, 0xF9};
    z2.insert(z2.end(), small.begin(), small.end());
    std::memset(nz.data(), 0, 0x3000);
    nz[0] = uint8_t(z2.size()); nz[1] = uint8_t(z2.size() >> 8);
    std::memcpy(&nz[2], z2.data(), z2.size());
    std::memcpy(&nz[0x3060], "2.0", 3);
    NacpInfo j = parse_nacp(nz.data(), nz.size());
    CHECK(j.table_was_compressed && j.title == std::string("Игра") && j.title_lang == std::string("AmericanEnglish") &&
              j.display_version == std::string("2.0"),
          "NACP: сжатая таблица распакована, название читается");
    CHECK(throws([&] { parse_nacp(n.data(), 0x3000); }, "NACP_SHORT"), "негатив: NACP короче 0x3070 — отказ");
    CHECK(sanitize_utf8("a\tb\x01\xFF" "c", 16) == "a bc", "sanitize_utf8: управляющие и битый UTF-8 убраны");
}

static void test_names_versions() {
    CHECK(safe_folder_name("CON") == "_CON", "имя папки: CON -> _CON");
    CHECK(safe_folder_name("a<b>c:d") == "a b c d", "имя папки: запрещённые символы -> пробел");
    CHECK(safe_folder_name("...") == "game", "имя папки: одни точки -> запасное");
    CHECK(safe_folder_name("  Zelda.  ") == "Zelda", "имя папки: хвост из точек и пробелов");
    CHECK(safe_folder_name(std::string(100, 'x')).size() == 80, "имя папки: не длиннее 80");
    CHECK(safe_folder_name("lpt1.txt") == "_lpt1.txt", "имя папки: lpt1.txt зарезервировано");
    uint8_t hdr[0x400] = {0};
    std::memcpy(hdr + 0x200, "NCA3", 4);
    wr32(hdr + 0x21C, 0x16020000);
    auto v = nca_sdk_addon_version(hdr, sizeof hdr);
    CHECK(v && SdkAddonVersion::from_u32(*v).text() == "22.2.0", "NCA: sdk_addon_version 0x16020000 -> 22.2.0");
    hdr[0x200] = 'X';
    CHECK(!nca_sdk_addon_version(hdr, sizeof hdr), "негатив: заголовок NCA без магии (не расшифрован) — нет версии");
    CHECK(hos_version_text(0x58500000) == "22.5.0", "required_system_version 0x58500000 -> 22.5.0");
    CHECK(slot_number("subsdk7") == 7 && slot_number("subsdk") == -1 && slot_number("subsdk10") == -1, "slot_number");
    CHECK(title_dir_of("sdmc:/", "0100abc000000000") == "sdmc:/atmosphere/contents/0100ABC000000000", "title_dir_of");
    CHECK(autofix_catalog().size() == 4 && autofix_catalog()[2].id == "nvrm_query" && autofix_catalog()[2].kind == "STUB" &&
              autofix_catalog()[2].scope == "NO_VULKAN" && autofix_catalog()[3].id == "nifm_access_status" &&
              autofix_catalog()[3].symbol == "_ZN2nn4nifm22GetNetworkAccessStatusEv" &&
              autofix_catalog()[3].kind == "MISSING" && autofix_catalog()[3].scope == "ANY",
          "каталог автоисправления: 4 записи из того же X-макроса");
    CHECK(is_return_const_stub(0x52800040, 0xD65F03C0) && !is_return_const_stub(0x52800040, 0xD65F03C1),
          "признак заглушки mov w0,#N; ret");
}

static void test_rules() {
    using V = std::array<int, 3>;
    V h19{19, 0, 1}, h22{22, 1, 0}, a111{1, 11, 1}, a112{1, 12, 0};
    CHECK(zbic_need(&h19, &a111, false).verdict == "not_compressed", "ZBIC: нет сжатия");
    CHECK(zbic_need(&h22, nullptr, true).verdict == "not_needed_hos22", "ZBIC: HOS 22 -> не нужно");
    CHECK(zbic_need(&h19, &a112, true).verdict == "not_needed_ams112", "ZBIC: HOS 19 + AMS 1.12.0 -> не нужно");
    CHECK(zbic_need(&h19, &a111, true).verdict == "needed", "ZBIC: HOS 19 + AMS 1.11.1 -> нужно");
    CHECK(zbic_need(&h19, nullptr, true).verdict == "needed", "ZBIC: AMS неизвестна -> нужно");
    CHECK(zbic_need(nullptr, nullptr, true).text_key == "core/zbic/needed", "ZBIC: ключ текста");

    SdkStatusInput in;
    in.firmware = V{19, 0, 1};
    in.game_sdk_exefs = V{22, 2, 0};
    in.available = {V{20, 5, 17}, V{19, 3, 13}, V{22, 2, 0}};
    auto s = evaluate_sdk_status(in);
    CHECK(s.verdict == "needed" && s.recommended == V({19, 3, 13}), "sdk: HOS 19 + игра 22 -> нужна, 19.3.13 (проверено)");
    in.available = {};
    s = evaluate_sdk_status(in);
    CHECK(s.verdict == "needed" && s.text_key == "core/sdk/needed_missing", "sdk: нужна, но подходящей нет на карте");
    in.game_sdk_exefs = V{15, 3, 1};
    CHECK(evaluate_sdk_status(in).verdict == "not_needed", "sdk: игра 15.3.1 на HOS 19 -> не нужна");
    in.game_sdk_exefs = V{22, 2, 0};
    in.swapped_present = true;
    in.swapped_version = V{19, 3, 13};
    CHECK(evaluate_sdk_status(in).verdict == "swapped_ok", "sdk: подменена 19.3.13 на HOS 19 -> ок");
    in.swapped_version = V{22, 2, 0};
    CHECK(evaluate_sdk_status(in).verdict == "swapped_mismatch", "sdk: подменена новее прошивки -> не так");
    in.game_sdk_exefs = V{15, 3, 1};
    in.swapped_version = V{19, 3, 13};
    CHECK(evaluate_sdk_status(in).text_key == "core/sdk/swapped_unneeded", "sdk: подмена не нужна игре -> не так");
    in = {};
    CHECK(evaluate_sdk_status(in).verdict == "unknown", "sdk: прошивка неизвестна -> unknown");
    CHECK(std::string(fw_sdk_table()[0].source).find("проверено") != std::string::npos, "таблица прошивка->sdk: источник у строки");
}

// Прогноз по готовым спискам символов (как в предпросмотре): Well Dweller × 19.3.13 / 22.2.0, SiN × 19.3.13.
static void test_forecast_parts() {
    using V = std::array<int, 3>;
    const std::string hm = "_ZNSt3__113__hash_memoryEPKvm", ldn = "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE",
                      nv = "NvRmMemQueryHandleParams";
    ForecastGame wd;
    wd.version = V{22, 2, 0};
    wd.opengl = true;
    wd.imports = {{nv, {"subsdk0"}}, {hm, {"main"}}};
    ForecastSdk s19;
    s19.name = "nnSdk";
    s19.known = true;
    s19.version = V{19, 3, 13};
    s19.exports = {{nv, true}};
    auto by = [](const ModuleForecast& m, const std::string& id) {
        for (auto& f : m.fixes)
            if (f.fix.id == id) return f;
        return FixForecast{};
    };
    auto m = forecast_fixes(wd, s19);
    CHECK(m.swapped && m.fixes.size() == autofix_catalog().size(), "прогноз: WD на 19.3.13 — подменена, все записи каталога");
    CHECK(by(m, "nvrm_query").choice == "own" && by(m, "nvrm_query").why == "StubOlderSdk", "прогноз: WD OpenGL -> своя");
    CHECK(by(m, "hash_memory").choice == "own" && by(m, "hash_memory").why == "SdkMissing", "прогноз: WD hash -> своя");
    CHECK(by(m, "ldn_set_protocol").importers.empty(), "прогноз: WD SetProtocol не импортирует");
    ForecastSdk s22 = s19;
    s22.version = V{22, 2, 0};
    s22.exports = {{nv, false}, {hm, false}, {ldn, false}};
    m = forecast_fixes(wd, s22);
    CHECK(!m.swapped && by(m, "nvrm_query").why == "SdkReal" && by(m, "hash_memory").why == "SdkDefined",
          "прогноз: WD на родной 22.2.0 -> прокси");
    ForecastGame sin = wd;
    sin.vulkan = true;
    sin.vulkan_module = "subsdk0";
    m = forecast_fixes(sin, s19);
    CHECK(by(m, "nvrm_query").choice == "proxy" && by(m, "nvrm_query").why == "StubVulkan", "прогноз: Vulkan -> графику не трогаем");
    // Shrek 2 Remastered (библиотеки 21.4.2): main зовёт nn::nifm::GetNetworkAccessStatus; в 19.3.13 символа нет.
    const std::string nifm = "_ZN2nn4nifm22GetNetworkAccessStatusEv";
    ForecastGame shrek;
    shrek.version = V{21, 4, 2};
    shrek.imports = {{nifm, {"main"}}};
    m = forecast_fixes(shrek, s19);
    CHECK(by(m, "nifm_access_status").choice == "own" && by(m, "nifm_access_status").why == "SdkMissing" &&
              by(m, "nifm_access_status").importers == std::vector<std::string>({"main"}),
          "прогноз: Shrek 2 на 19.3.13 — nifm своя (0)");
    ForecastSdk s21 = s19;
    s21.version = V{21, 4, 2};
    s21.exports = {{nifm, false}};
    m = forecast_fixes(shrek, s21);
    CHECK(by(m, "nifm_access_status").choice == "proxy" && by(m, "nifm_access_status").why == "SdkDefined",
          "прогноз: родная 21.4.2 — nifm прокси");
    // Пробел sdk для nifm теперь известен каталогу (не «не восполнит»).
    ForecastGame sg = shrek;
    sg.all_imports = {{nifm, {false, {"main"}}}};
    sg.own_sdk = {{nifm, false}};
    auto gp = sdk_gaps(sg.all_imports, sg.module_exports, s19.exports, sg.own_sdk);
    CHECK(gp.size() == 1 && gp[0].fix && gp[0].fix->id == "nifm_access_status", "пробелы: nifm помечен записью каталога");
}

// Пробелы sdk (sdk_gaps) на синтетике: MISSING, STUB, каталог, закрытые модулями игры, слабые, нет в своей sdk.
static void test_sdk_gaps() {
    const std::string hm = "_ZNSt3__113__hash_memoryEPKvm";   // есть в каталоге (MISSING)
    std::map<std::string, GameImport> imps = {
        {"a_missing", {false, {"main"}}},             // в старой нет, в своей настоящая -> MISSING
        {"b_stub", {false, {"main", "subsdk0"}}},     // в старой заглушка, в своей настоящая -> STUB
        {"c_both_stub", {false, {"main"}}},           // заглушка и там, и там -> не пробел
        {"d_real", {false, {"main"}}},                // настоящая в старой -> не пробел
        {"e_game", {false, {"main"}}},                // закрыт экспортом модуля игры
        {"f_weak", {true, {"subsdk1"}}},              // слабый импорт, в старой нет
        {"g_not_sdk", {false, {"main"}}},             // нет и в своей sdk -> не от sdk
        {hm, {false, {"main"}}},                      // известен каталогу
    };
    std::set<std::string> gexp = {"e_game"};
    std::map<std::string, bool> old_sdk = {{"b_stub", true}, {"c_both_stub", true}, {"d_real", false}};
    std::map<std::string, bool> own = {{"a_missing", false}, {"b_stub", false}, {"c_both_stub", true}, {"d_real", false},
                                       {"e_game", false}, {"f_weak", false}, {hm, false}};
    auto gaps = sdk_gaps(imps, gexp, old_sdk, own);
    auto find = [&](const std::string& s) -> const SdkGap* {
        for (auto& g : gaps)
            if (g.symbol == s) return &g;
        return nullptr;
    };
    CHECK(gaps.size() == 4, "пробелы sdk: ровно четыре (a, b, f, hash)");
    CHECK(find("a_missing") && find("a_missing")->kind == "MISSING" && !find("a_missing")->weak && !find("a_missing")->fix,
          "пробелы sdk: нет в старой -> MISSING, не в каталоге");
    CHECK(find("b_stub") && find("b_stub")->kind == "STUB" && find("b_stub")->importers.size() == 2,
          "пробелы sdk: заглушка против настоящей -> STUB, оба модуля");
    CHECK(!find("c_both_stub") && !find("d_real"), "пробелы sdk: заглушка в обеих и настоящая в старой — не пробел");
    CHECK(!find("e_game"), "пробелы sdk: закрытый модулем игры исключён");
    CHECK(find("f_weak") && find("f_weak")->weak, "пробелы sdk: слабый импорт помечен");
    CHECK(!find("g_not_sdk"), "пробелы sdk: нет в своей sdk — не пробел sdk");
    CHECK(find(hm) && find(hm)->fix && find(hm)->fix->id == "hash_memory", "пробелы sdk: символ каталога помечен записью");
    int unfixed = 0;
    for (auto& g : gaps)
        if (!g.fix) ++unfixed;
    CHECK(unfixed == 3, "пробелы sdk: без каталога — три (известные исключены)");
    CHECK(std::is_sorted(gaps.begin(), gaps.end(), [](const SdkGap& x, const SdkGap& y) { return x.symbol < y.symbol; }),
          "пробелы sdk: порядок по имени");
    CHECK(sdk_gaps(imps, gexp, own, own).empty(), "пробелы sdk: своя sdk против себя — пусто");
    GameImport two;   // по умолчанию слабый, пока не встретился сильный
    CHECK(two.weak && two.importers.empty(), "пробелы sdk: GameImport по умолчанию слабый и пустой");
}

static void test_executor() {
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/mk-unit-XXXXXX";
    std::string root = mkdtemp(tmpl);   // временное — остаётся (ничего не удаляем)
    mkhost::StdFs sfs;
    mkhost::Clock clk;
    clk.fixed_iso = "2026-11-08T12:00:00";
    clk.fixed_stamp = "20261108-120000";
    Env env{sfs, soft_hash(), clk};
    std::string dest = root + "/sd", src = root + "/src";
    sfs.make_dirs(src);
    sfs.make_dirs(dest + "/atmosphere/contents/0100000000001000/exefs");
    auto w = [&](const std::string& p, const std::string& t) { sfs.write_file_replace(p, (const uint8_t*)t.data(), t.size()); };
    w(src + "/a", "AAA");
    w(src + "/b", "BBB");
    std::string ex = dest + "/atmosphere/contents/0100000000001000/exefs";
    w(ex + "/main", "foreign main");
    w(ex + "/keep", "keep me");
    Plan p;
    p.stamp = clk.stamp();
    p.dest = dest;
    p.guard_dir = ex;
    p.foreign_before = {{"keep", sha256_hex(soft_hash(), Bytes{'k', 'e', 'e', 'p', ' ', 'm', 'e'})}};
    Action a;
    a.kind = ActKind::PutFile;
    a.src = src + "/a";
    a.dst = ex + "/main";
    a.rel = "atmosphere/contents/0100000000001000/exefs/main";
    a.replaces_foreign = true;
    p.actions.push_back(a);
    Action c;
    c.kind = ActKind::PutFile;
    c.src = src + "/b";
    c.dst = dest + "/atmosphere/contents/0100000000001000/files/x/y/b";
    c.rel = "atmosphere/contents/0100000000001000/files/x/y/b";
    p.actions.push_back(c);
    ReceiptFields rf;
    rf.tid = "0100000000001000";
    rf.zbic_files = {"main"};
    Action r;
    r.kind = ActKind::PutReceipt;
    r.dst = dest + "/atmosphere/contents/0100000000001000/nxredirect.json";
    r.receipt = make_receipt(rf, {});
    p.actions.push_back(r);
    ApplyReport rep = apply_plan(env, p);
    CHECK(rep.problems.empty() && sfs.is_file(ex + "/main.bak-20261108-120000"), "исполнитель: чужой файл отложен в .bak");
    CHECK(rep.receipt["installed"] == "2026-11-08T12:00:00" && rep.receipt["foreign_backups"].contains(a.rel) &&
              rep.receipt["owned"].size() == 2 && rep.receipt["created_dirs"].size() == 3,
          "исполнитель: квитанция учла owned/foreign_backups/created_dirs (как Ledger эталона)");
    // снятие: удалить наше, вернуть чужое, убрать созданные папки
    Plan u = plan_uninstall(env, dest, "0100000000001000", true);
    ApplyReport ur = apply_plan(env, u);
    auto rd = [&](const std::string& f) { Bytes b = sfs.read_file(f); return std::string(b.begin(), b.end()); };
    CHECK(sfs.is_file(ex + "/main") && rd(ex + "/main") == "foreign main", "снятие: чужой main вернулся на место");
    CHECK(!sfs.exists(dest + "/atmosphere/contents/0100000000001000/files") &&
              !sfs.exists(dest + "/atmosphere/contents/0100000000001000/nxredirect.json") && rd(ex + "/keep") == "keep me",
          "снятие: наше удалено, пустые наши папки убраны, чужое цело");
    // изменённый после установки файл не удаляется
    p.actions.erase(p.actions.begin());   // без main
    apply_plan(env, p);
    w(dest + "/atmosphere/contents/0100000000001000/files/x/y/b", "changed");
    Plan u2 = plan_uninstall(env, dest, "0100000000001000", true);
    bool warned = false;
    for (auto& s : u2.warnings) warned |= s.find("изменён") != std::string::npos;
    apply_plan(env, u2);
    CHECK(warned && rd(dest + "/atmosphere/contents/0100000000001000/files/x/y/b") == "changed",
          "снятие: изменённый после установки файл НЕ удалён, в плане предупреждение");
    // ключи перевода: у каждой строки плана и предупреждения есть ключ, параметры — путь
    bool keyed = !u2.lines.empty();
    for (size_t i = 0; i < u2.lines.size(); ++i) keyed &= !u2.lines.msg(i).key.empty();
    bool wkey = false;
    for (size_t i = 0; i < u2.warnings.size(); ++i)
        wkey |= u2.warnings.msg(i).key == "warn/un_changed" && u2.warnings.msg(i).args.size() == 1 &&
                u2.warnings.msg(i).args[0].find("files/x/y/b") != std::string::npos;
    CHECK(keyed && wkey, "перевод: строки плана снятия несут ключ и параметры (warn/un_changed + путь)");
    bool dkey = !ur.done.empty();
    for (size_t i = 0; i < ur.done.size(); ++i) dkey &= ur.done.msg(i).key.rfind("done/", 0) == 0;
    CHECK(dkey, "перевод: итог записи несёт ключи done/*");
    Texts raw;
    raw.push_back("без ключа");
    raw.add("с ключом", "plan/un_dirs");
    CHECK(raw.msg(0).key.empty() && raw.msg(1).key == "plan/un_dirs" && raw.size() == 2,
          "перевод: строка без ключа остаётся как есть, следующая с ключом не сдвигается");
    // гейт исполнителя: чужой файл тронут -> красный, квитанция не пишется
    Plan g;
    g.stamp = clk.stamp();
    g.dest = dest;
    g.guard_dir = ex;
    g.foreign_before = {{"keep", "0000"}};
    Action rr = r;
    rr.dst = root + "/never.json";
    g.actions.push_back(rr);
    CHECK(throws([&] { apply_plan(env, g); }, "FAIL_GATE_APPLY") && !sfs.exists(root + "/never.json"),
          "негатив: гейт сохранности чужого краснеет, квитанция не пишется");
    CHECK(throws([&] { plan_uninstall(env, root + "/nowhere", "0100000000001000"); }, "FAIL_UNINSTALL_NO_DIR"),
          "негатив: снимать нечего — отказ");
    try {
        plan_uninstall(env, root + "/nowhere", "0100000000001000");
    } catch (const Error& e) {
        CHECK(e.msg().key == "err/FAIL_UNINSTALL_NO_DIR" && e.args().size() == 1 &&
                  e.args()[0].find("nowhere") != std::string::npos,
              "перевод: ошибка ядра несёт ключ err/<код> и параметр-путь");
    }
}

// Как эталон: свои файлы при переустановке — перезапись начисто, без .bak;
// own_backups не пополняется; старые записи own_backups переносятся и удаляются снятием; чужие — .bak как прежде.
static void test_own_no_bak() {
    char tmpl[] = "/tmp/mk-unit-own-XXXXXX";
    std::string root = mkdtemp(tmpl);   // временное — остаётся (ничего не удаляем)
    mkhost::StdFs sfs;
    mkhost::Clock clk;
    clk.fixed_iso = "2026-11-09T12:00:00";
    clk.fixed_stamp = "20261109-120000";
    Env env{sfs, soft_hash(), clk};
    auto w = [&](const std::string& p, const std::string& t) { sfs.write_file_replace(p, (const uint8_t*)t.data(), t.size()); };
    auto txt = [](const std::string& t) { return Bytes(t.begin(), t.end()); };
    const std::string T = "0100000000002000", dest = root + "/sd", src = root + "/src";
    const std::string td = dest + "/atmosphere/contents/" + T, ex = td + "/exefs", rel = "atmosphere/contents/" + T;
    sfs.make_dirs(src);
    sfs.make_dirs(ex);
    w(src + "/m1", "module plain");
    w(src + "/m2", "module diag");
    w(src + "/n1", "npdm ours");
    w(ex + "/main.npdm", "npdm of the game (foreign)");
    auto run = [&](const std::string& mod, bool diag, const std::string& stamp) {
        clk.fixed_stamp = stamp;
        ReceiptRead base = read_receipt(env, td);
        Plan p;
        p.stamp = clk.stamp();
        p.dest = dest;
        if (!base.sane()) {
            Action o;
            o.kind = ActKind::SaveOrig;
            o.src = ex + "/main.npdm";
            o.dst = ex + "/main.npdm.orig";
            p.actions.push_back(o);
        }
        Action a;
        a.kind = ActKind::PutFile;
        a.src = mod;
        a.dst = ex + "/x9";
        a.ours_names.assign({"x9"});
        p.actions.push_back(a);
        Action n;
        n.kind = ActKind::PutFile;
        n.src = src + "/n1";
        n.dst = ex + "/main.npdm";
        n.ours_if_owned = ex + "/main.npdm.orig";
        p.actions.push_back(n);
        ReceiptFields rf;
        rf.tid = T;
        // слот не пишется: подставной модуль не NSO, а снятие проверяет, что в слоте квитанции — наш модуль
        rf.diag = diag;
        rf.module_variant = std::string(diag ? "autofix-diag" : "autofix");
        Action r;
        r.kind = ActKind::PutReceipt;
        r.dst = td + "/nxredirect.json";
        r.receipt = make_receipt(rf, base);
        r.receipt_base = base.sane() ? base.data : ojson::object();
        r.installed_by = "console";
        p.actions.push_back(r);
        return apply_plan(env, p);
    };
    run(src + "/m1", false, "20261109-120000");
    run(src + "/m2", true, "20261109-120100");
    ApplyReport last = run(src + "/m1", false, "20261109-120200");
    std::vector<std::string> baks;
    for (auto& e : sfs.list_dir(ex))
        if (e.name.find(".bak-") != std::string::npos) baks.push_back(e.name);
    for (auto& e : sfs.list_dir(td))
        if (e.name.find(".bak-") != std::string::npos) baks.push_back(e.name);
    CHECK(baks.empty() && sfs.read_file(ex + "/x9") == txt("module plain"),
          "свои без .bak: обычная -> отладочная -> обычная — ни одной копии .bak (модуль, npdm, квитанция)");
    CHECK(last.receipt["own_backups"].empty() && last.receipt["foreign_backups"].empty() &&
              last.receipt["module_variant"] == "autofix" && last.receipt["diag"] == false &&
              last.receipt.back() == "console",
          "свои без .bak: own_backups пуст, чужой main.npdm — в .orig (не .bak), вид сборки autofix, installed_by в конце");
    CHECK(throws([&] { sfs.write_file(ex + "/x9", reinterpret_cast<const uint8_t*>("z"), 1); }, "IO_EXISTS"),
          "негатив: запись поверх файла без явной перезаписи — отказ (как FAT/exFAT консоли)");
    // старая квитанция с own_backups: записи переносятся, снятие удаляет копии по sha256
    w(ex + "/x9.bak-20261101-000000", "old own copy");
    {
        ojson d = read_receipt(env, td).data;
        d["own_backups"][rel + "/exefs/x9.bak-20261101-000000"] = sha256_hex(soft_hash(), txt("old own copy"));
        w(td + "/nxredirect.json", dump_receipt(d));
    }
    ApplyReport again = run(src + "/m2", true, "20261109-120300");
    CHECK(again.receipt["own_backups"].size() == 1 && again.receipt["module_variant"] == "autofix-diag",
          "старые own_backups переносятся в новую квитанцию (не пополняются)");
    Plan u = plan_uninstall(env, dest, T, true);
    apply_plan(env, u);
    CHECK(!sfs.exists(ex + "/x9.bak-20261101-000000") && !sfs.exists(ex + "/x9") &&
              sfs.read_file(ex + "/main.npdm") == txt("npdm of the game (foreign)") && !sfs.exists(td + "/nxredirect.json"),
          "снятие: старая копия из own_backups удалена, чужой main.npdm вернулся из .orig");
    // подмена sdk поверх установки: учёт своего и installed_by сохраняются
    run(src + "/m1", false, "20261109-120400");
    SdkFile sk;
    sk.path = src + "/m2";
    sk.nnsdk = true;
    sk.module_name = "nnSdk";
    sk.sha256 = file_sha256(env, sk.path);
    sk.size = 11;
    Plan sp = plan_swap_sdk(env, dest, T, sk);
    ApplyReport sr = apply_plan(env, sp);
    CHECK(sr.receipt["owned"].contains(rel + "/exefs/x9") && sr.receipt["owned"].contains(rel + "/exefs/sdk") &&
              sr.receipt.value("installed_by", "") == "console",
          "подмена sdk: owned установки и installed_by не теряются (receipt_base)");
    Plan us = plan_unswap_sdk(env, dest, T);
    ApplyReport ur = apply_plan(env, us);
    CHECK(ur.receipt["owned"].contains(rel + "/exefs/x9") && !ur.receipt["owned"].contains(rel + "/exefs/sdk") &&
              !sfs.exists(ex + "/sdk"),
          "снятие подмены sdk: sdk убран из owned, прочий учёт своего цел");
}

// Логи по играм и «Собрать логи для отправки» (mk::logs): подставные файлы с датами и именами.
static void test_logs() {
    namespace L = mk::logs;
    char tmpl[] = "/tmp/mk-unit-logs-XXXXXX";
    std::string root = mkdtemp(tmpl);   // временное — остаётся (ничего не удаляем)
    mkhost::StdFs sfs;
    mkhost::Clock clk;
    clk.fixed_iso = "2026-11-09T15:30:00";
    clk.fixed_stamp = "20261109-153000";
    Env env{sfs, soft_hash(), clk};
    auto w = [&](const std::string& p, const std::string& t) {
        sfs.make_dirs(path_parent(p));
        sfs.write_file_replace(p, (const uint8_t*)t.data(), t.size());
    };
    const std::string T = "0100217023F6C000", other = "0100F07026DA8000";
    const std::string sd = root + "/sd", logs = sd + "/switch/ModKitNX/logs/", crash = sd + "/atmosphere/crash_reports/";
    std::string k;
    uint32_t n = 0;
    CHECK(L::parse_report_name("diag-0005.txt", k, n) && k == "diag" && n == 5 &&
              L::parse_report_name("gfx-10000.txt", k, n) && k == "gfx" && n == 10000 &&
              !L::parse_report_name("diag-05.txt", k, n) && !L::parse_report_name("diag-0005.log", k, n) &&
              !L::parse_report_name("nxredirect-diag.txt", k, n) && !L::parse_report_name("diag-00a5.txt", k, n),
          "логи: имя отчёта diag-NNNN.txt / gfx-NNNN.txt (не меньше 4 цифр)");
    const std::string bom = "\xEF\xBB\xBF";
    for (int i : {3, 4, 5, 6, 7}) {
        char nm[32];
        std::snprintf(nm, sizeof(nm), "diag-%04d.txt", i);
        w(logs + T + "/" + nm, bom + "TitleID=" + T + " BuildID=? report=diag-" + std::string(nm).substr(5, 4) + "\r\nтекст\r\n");
    }
    w(logs + T + "/gfx-0007.txt", bom + "TitleID=" + T + " BuildID=AB report=gfx-0007\r\n");
    w(logs + T + "/diag-0099.txt.part", "недописанный");
    auto rep = L::list_reports(sfs, logs, T, "diag");
    CHECK(rep.size() == 5 && rep[0].number == 7 && rep[4].number == 3 && L::list_reports(sfs, logs, other, "diag").empty(),
          "логи: отчёты игры — от нового к старому, .part и чужая игра не считаются");
    L::ReportHead h = L::parse_report_head(bom + "TitleID=0100217023f6c000 BuildID=0123456789abcdef0123456789abcdef01234567 report=diag-0007\r\nx");
    CHECK(h.ok && h.tid == T && h.build_id.size() == 40 && h.report == "diag-0007" && !L::parse_report_head("nxredirect —").ok,
          "логи: первая строка TitleID/BuildID/report (BOM пропускается)");
    // отчёты о падении: дата изменения, затем число в начале имени; TID из имени, иначе из текста
    std::map<std::string, int64_t> mt;
    auto crashf = [&](const std::string& name, const std::string& text, std::optional<int64_t> t) {
        w(crash + name, text);
        if (t) mt[crash + name] = *t;
    };
    crashf("01700000000_" + T + ".log", "Program ID: " + T, 1700000300);
    crashf("01700000100_" + T + ".log", "Program ID: " + T, 1700000200);    // число больше, дата старше
    crashf("01700000500_" + other + ".log", "Program ID: " + other, 1700009999);   // другая игра
    crashf("report_without_tid.log", "Atmosphere Crash Report\n    Program ID:                  " +
                                         std::string("0100217023f6c000") + "\n", 1700000250);
    crashf("01700000900_" + T + ".txt", "не .log", 1800000000);
    L::MtimeFn mf = [&](const std::string& p) -> std::optional<int64_t> {
        auto it = mt.find(p);
        if (it == mt.end()) return std::nullopt;
        return it->second;
    };
    auto rd = [&](const std::string& p) { Bytes b = sfs.read_file(p); return std::string(b.begin(), b.end()); };
    auto files = L::list_crash_files(sfs, crash, mf);
    auto last = L::pick_last_crash(files, T, rd);
    CHECK(files.size() == 4 && last && last->name == "01700000000_" + T + ".log",
          "падение: последний по ДАТЕ изменения (число в имени меньше), только .log, только эта игра");
    mt[crash + "01700000100_" + T + ".log"] = 1700000300;   // дата равна — решает число
    last = L::pick_last_crash(L::list_crash_files(sfs, crash, mf), T, rd);
    CHECK(last && last->name == "01700000100_" + T + ".log", "падение: при равной дате — по числу в начале имени");
    mt.erase(crash + "01700000100_" + T + ".log");   // у одного нет даты — по числу
    last = L::pick_last_crash(L::list_crash_files(sfs, crash, mf), T, rd);
    CHECK(last && last->name == "01700000100_" + T + ".log", "падение: без даты у кого-то — по числу в начале имени");
    mt[crash + "01700000100_" + T + ".log"] = 1700000000;
    mt[crash + "report_without_tid.log"] = 1700099999;
    last = L::pick_last_crash(L::list_crash_files(sfs, crash, mf), T, rd);
    CHECK(last && last->name == "report_without_tid.log" && L::crash_tid_from_name("report_without_tid.log").empty(),
          "падение: TitleID не в имени — из текста (Program ID)");
    CHECK(!L::pick_last_crash(files, "0100000000009999", rd), "падение: у игры отчётов нет — пусто");
    // сборка
    w(sd + "/atmosphere/contents/" + T + "/nxredirect.json", "{\n  \"game_tid\": \"" + T + "\"\n}\n");
    w(sd + "/atmosphere/contents/" + T + "/exefs/main", "game code");
    w(sd + "/atmosphere/contents/" + T + "/nxredirect-passport.txt", "паспорт модуля\r\n");
    w(sd + "/switch/prod.keys", "secret");
    L::BundleInput in;
    in.tid = T;
    in.game_name = "Well Dweller: ремастер?";
    in.game_version = "1.0.3";
    in.logs_root = logs;
    in.crash_dir = crash;
    in.contents_dir = sd + "/atmosphere/contents/";
    in.legacy_diag = sd + "/nxredirect-diag.txt";
    in.passport_text = bom + "паспорт приложения\r\n";
    L::Bundle b = L::plan_logs_bundle(env, in, mf);
    std::vector<std::string> names;
    for (auto& it : b.items) names.push_back(it.name);
    const std::vector<std::string> want = {"diag-0007.txt", "diag-0006.txt", "diag-0005.txt", "diag-0004.txt", "diag-0003.txt",
                                           "gfx-0007.txt", "crash-report_without_tid.log", "passport-module.txt",
                                           "passport.txt", "nxredirect.json"};
    CHECK(names == want, "сборка: отчёты diag/gfx, последний crash, оба паспорта (модуля и приложения), квитанция — ничего лишнего");
    CHECK(b.inner == "Well Dweller ремастер v1.0.3" && b.zip == logs + "Well Dweller ремастер v1.0.3 20261109-153000.zip" &&
              b.folder == logs + "Well Dweller ремастер v1.0.3/",
          "сборка: имя папки (безопасное для FAT) и архива с датой");
    L::BundleResult r = L::write_logs_bundle(env, b);
    Bytes z = sfs.read_file(b.zip);
    auto u32 = [&](size_t o) { return uint32_t(z[o]) | uint32_t(z[o + 1]) << 8 | uint32_t(z[o + 2]) << 16 | uint32_t(z[o + 3]) << 24; };
    auto u16 = [&](size_t o) { return uint32_t(z[o]) | uint32_t(z[o + 1]) << 8; };
    bool zok = z.size() > 22 && u32(z.size() - 22) == 0x06054b50 && u16(z.size() - 22 + 10) == want.size();
    size_t off = 0, cnt = 0;
    std::string firstName;
    while (zok && off + 30 < z.size() && u32(off) == 0x04034b50) {
        const uint32_t crc = u32(off + 14), sz = u32(off + 18), nl = u16(off + 26);
        const std::string nm(z.begin() + off + 30, z.begin() + off + 30 + nl);
        if (!cnt) firstName = nm;
        zok &= u16(off + 8) == 0 && (u16(off + 6) & 0x0800) && L::crc32(z.data() + off + 30 + nl, sz) == crc &&
               nm.rfind(b.inner + "/", 0) == 0;
        off += 30 + nl + sz;
        cnt++;
    }
    CHECK(zok && cnt == want.size() && r.files == want.size() && firstName == b.inner + "/diag-0007.txt" &&
              sfs.is_file(b.folder + "crash-report_without_tid.log") && rd(b.folder + "passport.txt") == in.passport_text &&
              rd(b.folder + "passport-module.txt") == "паспорт модуля\r\n",
          "архив: store, имена UTF-8, CRC сходится, всё внутри папки игры; копии лежат в папке");
    bool leak = false;
    for (size_t i = 0; i + 6 <= z.size(); ++i) leak |= std::equal(z.begin() + i, z.begin() + i + 6, "secret");
    for (auto& nm : names) leak |= nm == "main" || nm.find("keys") != std::string::npos;
    CHECK(!leak, "архив: ни ключей, ни файлов игры");
    CHECK(L::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "crc32: эталон 123456789");
    uint16_t dt = 0, dd = 0;
    L::dos_from_stamp("20261109-153000", dt, dd);
    CHECK(dd == ((46 << 9) | (11 << 5) | 9) && dt == ((15 << 11) | (30 << 5)), "zip: дата DOS из штампа");
    // второй сбор поверх (свои файлы в папке перезаписываются), нет отчётов — предупреждения, старый отчёт в корне
    clk.fixed_stamp = "20261109-160000";
    L::Bundle b2 = L::plan_logs_bundle(env, in, mf);
    L::write_logs_bundle(env, b2);
    CHECK(sfs.is_file(b2.zip) && sfs.is_file(b.zip), "сборка: повторный сбор — новый архив, папка перезаписана");
    // журнал чтения exefs (тексты приложения) — отдельным файлом; пустые не кладутся
    L::BundleInput in4 = in;
    in4.app_texts.emplace_back("exefs-read.txt", "exefs: step header ok");
    in4.app_texts.emplace_back("empty.txt", "");
    L::Bundle bApp = L::plan_logs_bundle(env, in4, mf);
    size_t appItems = 0;
    for (auto& it : bApp.items) appItems += it.what == "app";
    CHECK(appItems == 1 && bApp.items.size() == want.size() + 1 && bApp.items[want.size() - 1].name == "exefs-read.txt",
          "сборка: журнал чтения exefs — один файл перед квитанцией, пустой текст не кладётся");
    L::BundleInput in2 = in;
    in2.tid = other;
    w(sd + "/nxredirect-diag.txt", "nxredirect — диагностический отчёт\nTitleID (svcGetInfo ProgramId) = 0x" + other + "\n");
    L::Bundle b3 = L::plan_logs_bundle(env, in2, mf);
    bool legacy = false, noReceipt = false;
    for (auto& it : b3.items) legacy |= it.name == "nxredirect-diag.txt";
    for (size_t i = 0; i < b3.warnings.size(); ++i) noReceipt |= b3.warnings.msg(i).key == "logs/no_receipt";
    CHECK(legacy && noReceipt, "сборка: старый sdmc:/nxredirect-diag.txt этой игры берётся как запасной");
    L::Bundle b4 = L::plan_logs_bundle(env, in, mf);
    bool legacyT = false;
    for (auto& it : b4.items) legacyT |= it.name == "nxredirect-diag.txt";
    CHECK(!legacyT, "сборка: есть новые отчёты — старый файл в корне не берётся");
}

// ------------------------------------------------------------ «Найти sdk в других играх» (mk::harvest)
// Синтетический nnSdk: NSO0 без сжатия, MOD0 + .dynamic, имя модуля и строка SDK MW в rodata.
static Bytes make_sdk_nso(const std::string& module, const std::string& ver_us, uint8_t id) {
    Bytes text(0x100, 0), ro, data(0x10, 0);
    wr32(&text[4], 0x10);
    std::memcpy(&text[0x10], "MOD0", 4);
    wr32(&text[0x14], 0x30);   // .dynamic по 0x40
    const uint64_t tags[] = {5, 0, 10, 0, 6, 0, 11, 24, 0, 0};
    for (int i = 0; i < 10; i++) wr64(&text[0x40 + i * 8], tags[i]);
    ro.resize(8, 0);
    wr32(&ro[4], uint32_t(module.size()));
    ro.insert(ro.end(), module.begin(), module.end());
    ro.resize(ro.size() + 8, 0);
    std::string mw = "SDK MW+Nintendo+NintendoSdk_" + module + "-" + ver_us + "-Release";
    ro.insert(ro.end(), mw.begin(), mw.end());
    ro.resize((ro.size() + 0x10) & ~size_t(0xF), 0);
    Bytes n(0x100, 0);
    std::memcpy(n.data(), "NSO0", 4);
    uint32_t off = 0x100, mem = 0;
    Bytes* segs[3] = {&text, &ro, &data};
    for (int i = 0; i < 3; i++) {
        wr32(&n[0x10 + i * 0x10], off);
        wr32(&n[0x14 + i * 0x10], mem);
        wr32(&n[0x18 + i * 0x10], uint32_t(segs[i]->size()));
        wr32(&n[0x60 + i * 4], uint32_t(segs[i]->size()));
        off += uint32_t(segs[i]->size());
        mem += uint32_t((segs[i]->size() + 0xFFF) & ~size_t(0xFFF));
    }
    n[0x40] = id;
    for (Bytes* s : segs) n.insert(n.end(), s->begin(), s->end());
    return n;
}

static void test_harvest() {
    namespace H = mk::harvest;
    using V = std::array<int, 3>;
    char tmpl[] = "/tmp/mk-unit-harvest-XXXXXX";
    std::string root = mkdtemp(tmpl);   // временное — остаётся (ничего не удаляем)
    mkhost::StdFs sfs;
    mkhost::Clock clk;
    clk.fixed_iso = "2026-11-09T12:00:00";
    Env env{sfs, soft_hash(), clk};
    auto put = [&](const std::string& p, const Bytes& b) {
        sfs.make_dirs(path_parent(p));
        sfs.write_file_replace(p, b.data(), b.size());
    };
    // игры: две на одном и том же sdk 19.3.13, одна на 18.3.0, одна на 22.2.0, одна не nnSdk, одна без sdk
    const Bytes s19 = make_sdk_nso("nnSdk", "19_3_13", 1), s19b = make_sdk_nso("nnSdk", "19_3_13", 2),
                s18 = make_sdk_nso("nnSdk", "18_3_0", 3), s22 = make_sdk_nso("nnSdk", "22_2_0", 4),
                gl = make_sdk_nso("opengl", "22_2_0", 5);
    put(root + "/g/cel/sdk", s19);
    put(root + "/g/cup/sdk", s19);
    put(root + "/g/old/sdk", s18);
    put(root + "/g/new/sdk", s22);
    put(root + "/g/gl/sdk", gl);
    sfs.make_dirs(root + "/g/none");
    auto base = [](const std::string& tid, const std::string& name, const std::string& ver) {
        H::GameSdk g;
        g.tid = tid;
        g.name = name;
        g.version = ver;
        g.key = "key-" + tid;
        return g;
    };
    auto cel = H::probe_sdk_file(env, root + "/g/cel/sdk", base("01002B30028F6000", "Celeste", "1.4.0"));
    CHECK(cel.has_sdk && cel.nnsdk && cel.sdk_version == V({19, 3, 13}) && cel.sha256 == sha256_hex(soft_hash(), s19) &&
              !cel.zbic && cel.checked == "2026-11-09T12:00:00",
          "sdk из игры: nnSdk 19.3.13 опознана по содержимому, sha256 файла");
    auto none = H::probe_sdk_file(env, root + "/g/none/sdk", base("0100000000000001", "NoSdk", "1.0"));
    CHECK(!none.has_sdk, "sdk из игры: нет файла sdk — не ошибка, has_sdk=false");
    auto glr = H::probe_sdk_file(env, root + "/g/gl/sdk", base("0100000000000002", "GL", "1.0"));
    CHECK(glr.has_sdk && !glr.nnsdk && glr.module_name == "opengl", "негатив: NSO не nnSdk — не кандидат");
    std::vector<H::GameSdk> games = {
        cel,
        H::probe_sdk_file(env, root + "/g/cup/sdk", base("0100A00000000001", "Cuphead", "1.3.4")),
        H::probe_sdk_file(env, root + "/g/old/sdk", base("0100A00000000002", "Hades", "1.38")),
        H::probe_sdk_file(env, root + "/g/new/sdk", base("0100A00000000003", "Well Dweller", "1.0.3")),
        glr, none};

    // кэш: туда и обратно, промах при другом ключе, битый файл — пустой
    H::Cache c;
    for (auto& g : games) H::cache_put(c, g);
    H::Cache c2 = H::parse_cache(H::dump_cache(c));
    const H::GameSdk* hit = H::cache_hit(c2, cel.tid, cel.key);
    CHECK(c2.games.size() == games.size() && hit && hit->sdk_version == V({19, 3, 13}) && hit->sha256 == cel.sha256 &&
              hit->name == "Celeste" && hit->version == "1.4.0",
          "кэш sdk-scan.json: запись туда и обратно без потерь");
    CHECK(!H::cache_hit(c2, cel.tid, "другая версия игры"), "кэш: игра обновлена (другой ключ) — пересканировать");
    CHECK(H::parse_cache(Bytes{'{', 'x'}).games.empty() && H::parse_cache(Bytes{'{', '}'}).games.empty(),
          "кэш: битый файл или чужая схема — пустой кэш");
    {
        // Неверные типы полей: nlohmann бросал type_error мимо try → std::terminate. Теперь — пустой кэш.
        auto pc = [](const std::string& s) { return H::parse_cache(Bytes(s.begin(), s.end())); };
        const std::string h64(64, 'a');
        bool ok = true, threw = false;
        try {
            ok = pc(R"({"schema":"1","games":[]})").games.empty() && pc(R"({"schema":null,"games":[]})").games.empty() &&
                 pc(R"({"schema":1,"games":[{"title_id":"0100000000000001","key":"k","sdk":{"nnsdk":"yes","sha256":")" +
                    h64 + R"("}}]})")
                     .games.empty() &&
                 pc(R"({"schema":1,"games":[{"title_id":"0100000000000001","key":"k","sdk":{"size":"big","sha256":")" +
                    h64 + R"("}}]})")
                     .games.empty() &&
                 pc(R"({"schema":1,"games":[{"title_id":"0100000000000001","key":"k","sdk":{"zbic":null,"sha256":")" +
                    h64 + R"("}}]})")
                     .games.empty() &&
                 pc("[1,2]").games.empty() && pc("null").games.empty();
        } catch (...) {
            threw = true;
        }
        CHECK(!threw && ok, "кэш: неверные типы полей («\"schema\":\"1\"», null, строка вместо bool/числа) — пустой "
                            "кэш, без исключения");
        const std::string cbad = root + "/sd/bad-types/sdk-scan.json";
        const std::string t = R"({"schema":"1","games":[]})";
        put(cbad, Bytes(t.begin(), t.end()));
        threw = false;
        try {
            ok = H::load_cache(env, cbad).games.empty();
        } catch (...) {
            threw = true;
        }
        CHECK(!threw && ok, "кэш: load_cache подставного файла с неверными типами — пустой, без исключения");
    }
    {
        // Файл sdk в exefs есть, но не читается как NSO (в т.ч. временная ошибка чтения): не «sdk нет», а сбой —
        // платформа такую игру в кэш не кладёт и проверит в следующий раз.
        put(root + "/g/junk/sdk", Bytes(0x200, 0x5a));
        put(root + "/g/tiny/sdk", Bytes{1, 2, 3});
        CHECK(throws([&] { H::probe_sdk_file(env, root + "/g/junk/sdk", base("0100000000000003", "Junk", "1")); },
                     "SDK_UNREADABLE") &&
                  throws([&] { H::probe_sdk_file(env, root + "/g/tiny/sdk", base("0100000000000004", "Tiny", "1")); },
                         "SDK_UNREADABLE"),
              "sdk из игры: файл есть, но не NSO / не прочитан — SDK_UNREADABLE, а не запись «sdk нет» для кэша");
    }
    const std::string cpath = root + "/sd/switch/ModKitNX/cache/sdk-scan.json";
    H::save_cache(env, cpath, c2);
    H::save_cache(env, cpath, c2);   // свой файл — перезапись
    CHECK(H::load_cache(env, cpath).games.size() == games.size(), "кэш: сохранён в cache/ (папка создана), перечитан");

    // выбор для HOS 19.0.1
    auto sel = H::select_candidates(games, V{19, 0, 1}, {});
    CHECK(sel.candidates.size() == 1 && sel.candidates[0].version == V({19, 3, 13}) && sel.candidates[0].recommended &&
              sel.candidates[0].games.size() == 2 && sel.candidates[0].games[0].name == "Celeste" && sel.older_hidden == 1,
          "выбор: рекомендованная 19.3.13, дубли (по sha256) одной строкой с двумя играми; "
          "нижняя граница — 18.3.0 скрыта (есть sdk мажора прошивки)");
    CHECK(sel.too_new == 1 && sel.not_nnsdk == 1 && sel.with_nnsdk == 4 && sel.table_sdk == V({19, 3, 13}),
          "выбор: 22.2.0 — новее прошивки, opengl — не nnSdk; таблица ядра для HOS 19 = 19.3.13");
    auto sel2 = H::select_candidates(games, V{19, 0, 1}, {{cel.sha256, ""}});
    CHECK(sel2.candidates.size() == 1 && sel2.already == 2 && sel2.candidates[0].version == V({18, 3, 0}) &&
              sel2.older_hidden == 0,
          "выбор: sdk, что уже лежит в папке, не предлагается; мажора прошивки среди кандидатов нет — "
          "старшая 18.3.0 показана (последний шанс)");
    CHECK(H::select_candidates(games, std::nullopt, {}).candidates.empty(), "выбор: прошивка неизвестна — кандидатов нет");
    auto other = games;
    put(root + "/g/b/sdk", s19b);
    other.push_back(H::probe_sdk_file(env, root + "/g/b/sdk", base("0100A00000000005", "Build B", "2.0")));
    auto sel3 = H::select_candidates(other, V{20, 1, 0}, {});
    CHECK(sel3.candidates.size() == 3 && !sel3.candidates[0].recommended && sel3.candidates[0].version == V({19, 3, 13}) &&
              sel3.candidates[0].other_build && sel3.candidates[1].other_build && sel3.candidates[2].version == V({18, 3, 0}),
          "выбор: HOS 20 без 20.5.17 — ближайшая 19.3.13 первой; другая сборка той же версии — отдельной строкой");
    CHECK(sel3.older_hidden == 0, "нижняя граница: sdk мажора прошивки нет — ниже прошивки показаны все (последний шанс)");
    auto with20 = other;
    put(root + "/g/s20/sdk", make_sdk_nso("nnSdk", "20_1_5", 6));
    with20.push_back(H::probe_sdk_file(env, root + "/g/s20/sdk", base("0100A00000000006", "Sdk20", "1.0")));
    auto sel4 = H::select_candidates(with20, V{20, 1, 0}, {});
    CHECK(sel4.candidates.size() == 1 && sel4.candidates[0].version == V({20, 1, 5}) && sel4.older_hidden == 4,
          "нижняя граница: есть sdk мажора прошивки (20.1.5 при HOS 20) — 19.x и 18.x не показаны");

    // имя папки: FAT32-безопасно
    H::Candidate nc = sel.candidates[0];
    CHECK(H::folder_name(nc) == "nnSdk 19.3.13 (Celeste)", "имя папки: nnSdk 19.3.13 (Celeste)");
    nc.games[0].name = "Zelda: Tears/of \"the\" Kingdom?*";
    CHECK(H::folder_name(nc) == "nnSdk 19.3.13 (Zelda Tears of the Kingdom)", "имя папки: : / \" ? * убраны");
    nc.games[0].name = std::string(30, 'x') + "Ёжик в тумане и ещё очень длинное название";
    const std::string fn = H::folder_name(nc);
    CHECK(fn.size() < 120 && fn.back() == ')' && fn.find("Ёжик") != std::string::npos,
          "имя папки: длинное название обрезано по символам");

    // сохранение
    const std::string sdkdir = root + "/sd/switch/ModKitNX/sdk";
    auto r = H::save_candidate(env, sdkdir, sel.candidates[0], root + "/g/cel/sdk", "ModKitNX");
    CHECK(r.ok && r.target.dir == sdkdir + "/nnSdk 19.3.13 (Celeste)" && sfs.read_file(r.target.sdk_path) == s19,
          "сохранение: побайтная копия в sdk/nnSdk 19.3.13 (Celeste)/sdk");
    const Bytes sjb = sfs.read_file(r.target.source_path);
    auto sj = ojson::parse(std::string(sjb.begin(), sjb.end()));
    CHECK(sj["sdk_version"] == "19.3.13" && sj["game"]["name"] == "Celeste" && sj["game"]["title_id"] == "01002B30028F6000" &&
              sj["game"]["version"] == "1.4.0" && sj["sha256"] == cel.sha256 && sj["saved"] == "2026-11-09T12:00:00" &&
              sj["kind"] == "from_installed_game" && sj["also_in_games"].size() == 1,
          "source.json: версия, игра, TitleID, версия игры, sha256, дата, «из установленной игры»");
    auto src = H::read_source(env, r.target.sdk_path, cel.sha256);
    CHECK(src && src->game == "Celeste" && src->game_version == "1.4.0", "подпись «из игры Celeste» по source.json");
    CHECK(!H::read_source(env, r.target.sdk_path, std::string(64, '0')), "негатив: sha256 файла не тот — подписи нет");
    auto found = scan_sdk(env, {{sdkdir, "modkit"}});
    CHECK(found.size() == 1 && found[0].file.version == V({19, 3, 13}),
          "сохранённая видна обычному поиску sdk (source.json не мешает)");
    auto again = H::save_candidate(env, sdkdir, sel.candidates[0], root + "/g/cel/sdk", "ModKitNX");
    CHECK(again.ok && again.target.already && again.target.dir == r.target.dir,
          "повторное сохранение: уже лежит — ничего не пишется");
    // кандидат с чужим sha256 (раньше брался sel.candidates[1] при size()==1 — выход за границу, UB)
    H::Candidate changedCand = sel.candidates[0];
    changedCand.sha256 = std::string(64, '0');
    auto chg = H::save_candidate(env, sdkdir, changedCand, root + "/g/cel/sdk", "ModKitNX");
    CHECK(!chg.ok && chg.code == "changed", "негатив: файл в игре не тот, что найден (sha256) — не сохраняется");
    H::Candidate busy = sel3.candidates[1];   // другая сборка 19.3.13
    busy.games[0].name = "Celeste";
    auto r2 = H::save_candidate(env, sdkdir, busy, root + "/g/b/sdk", "ModKitNX");
    CHECK(r2.ok && r2.target.dir == sdkdir + "/nnSdk 19.3.13 (Celeste) 2" && sfs.read_file(r.target.sdk_path) == s19,
          "имя занято другим файлом — папка « 2», прежний файл не тронут");

    // настоящие sdk (только чтение, если задана папка MK_SDK_DIR; в репозиторий не кладутся)
    if (const char* d = std::getenv("MK_SDK_DIR")) {
        int real = 0;
        for (auto& e : sfs.list_dir(d)) {
            if (e.is_dir) continue;
            H::GameSdk g;
            try {
                g = H::probe_sdk_file(env, path_join(d, e.name), base("0100000000000009", "real", "1"));
            } catch (const Error&) {
                continue;   // не NSO (source.json и т.п.) — SDK_UNREADABLE
            }
            if (g.nnsdk && g.sdk_version) {
                real++;
                std::printf("  настоящая nnSdk %s\n", version_text(*g.sdk_version).c_str());
            }
        }
        CHECK(real > 0, "настоящие nnSdk из MK_SDK_DIR опознаны");
    } else {
        std::printf("  настоящие sdk: MK_SDK_DIR не задана — пропущено\n");
    }
}

// ------------------------------------------------------------ поиск символов nn::ro (прогноз ApplyRoFix модуля)
// Подставные данные: инструкции тех же видов, что в коде nnSdk (пролог, `ldr x0, [x1]`, `mov x1, x2`, `bl`; у F —
// `ldr x8, [x0, #80]` или `ldrb w9, [x0, #195]`), и синтетический NSO с одним экспортом.
namespace ro_t {
constexpr uint32_t Stp32 = 0xA9BE7BFD, Stp80 = 0xA9BB7BFD, StpX20 = 0xA9014FF4, MovX29Sp = 0x910003FD,
                   MovX19X0 = 0xAA0003F3, MovX20X1 = 0xAA0103F4, MovX19X1 = 0xAA0103F3, LdrX0X1 = 0xF9400020,
                   MovX1X2 = 0xAA0203E1, LdrX8At80 = 0xF9402808, LdrbW9At195 = 0x39430C09;
inline uint32_t Bl(uint64_t from, uint64_t to) { return 0x94000000u | (uint32_t((to - from) / 4) & 0x03FFFFFFu); }

// NSO0 без сжатия: код LMS по 0x100, F по 0x140, .dynsym по 0x180 (один символ sym, GLOBAL FUNC, значение 0x100),
// .dynstr по 0x200; gnu — DT_GNU_HASH в .dynamic (таблица на 1 ведро по 0x2C0).
Bytes make_nso(const std::vector<uint32_t>& lms, const std::vector<uint32_t>& f, const std::string& sym, bool gnu) {
    Bytes text(0x400, 0), ro(0x20, 0), data(0x10, 0);
    wr32(&text[4], 0x10);
    std::memcpy(&text[0x10], "MOD0", 4);
    wr32(&text[0x14], 0x30);   // .dynamic по 0x40
    std::vector<uint64_t> tags = {5, 0x200, 10, 0x80, 6, 0x180, 11, 24};
    if (gnu) tags.insert(tags.end(), {uint64_t(kDtGnuHash), 0x2C0});
    tags.insert(tags.end(), {0, 0});
    for (size_t i = 0; i < tags.size(); i++) wr64(&text[0x40 + i * 8], tags[i]);
    for (size_t i = 0; i < lms.size() && i < 16; i++) wr32(&text[0x100 + i * 4], lms[i]);
    for (size_t i = 0; i < f.size() && i < 12; i++) wr32(&text[0x140 + i * 4], f[i]);
    wr32(&text[0x198], 1);          // st_name
    text[0x19C] = 0x12;             // GLOBAL FUNC
    text[0x19E] = 1;                // st_shndx
    wr64(&text[0x1A0], 0x100);      // st_value
    wr64(&text[0x1A8], 0x40);       // st_size
    std::memcpy(&text[0x201], sym.data(), std::min<size_t>(sym.size(), 0x70));
    if (gnu) {
        wr32(&text[0x2C0], 1);      // nbuckets
        wr32(&text[0x2C4], 1);      // symoffset
        wr32(&text[0x2D0], 1);      // bucket[0]
        wr32(&text[0x2D4], 1);      // chain[0]: конец цепочки
    }
    const std::string nm = "nnSdk";
    wr32(&ro[4], uint32_t(nm.size()));
    std::memcpy(&ro[8], nm.data(), nm.size());
    Bytes n(0x100, 0);
    std::memcpy(n.data(), "NSO0", 4);
    uint32_t off = 0x100, mem = 0;
    Bytes* segs[3] = {&text, &ro, &data};
    for (int i = 0; i < 3; i++) {
        wr32(&n[0x10 + i * 0x10], off);
        wr32(&n[0x14 + i * 0x10], mem);
        wr32(&n[0x18 + i * 0x10], uint32_t(segs[i]->size()));
        wr32(&n[0x60 + i * 4], uint32_t(segs[i]->size()));
        off += uint32_t(segs[i]->size());
        mem += uint32_t((segs[i]->size() + 0xFFF) & ~size_t(0xFFF));
    }
    for (Bytes* s : segs) n.insert(n.end(), s->begin(), s->end());
    return n;
}
}  // namespace ro_t

static void test_ro_lookup() {
    using namespace ro_t;
    const uint64_t at = 0x14b180, fa = 0x14b1e0;
    const std::vector<uint32_t> lms = {Stp32, StpX20, MovX29Sp, MovX19X0, LdrX0X1, MovX20X1, MovX1X2, Bl(at + 7 * 4, fa)};
    CHECK(ro_internal_lookup(lms.data(), lms.size(), at) == fa, "nn::ro: F — цель первого bl после ldr x0,[x1] и mov x1,x2");
    auto noMov = lms;
    noMov[6] = MovX20X1;
    CHECK(ro_internal_lookup(noMov.data(), noMov.size(), at) == 0, "nn::ro: без mov x1,x2 перед bl — образец не узнан");
    auto noObj = lms;
    noObj[4] = MovX19X0;
    CHECK(ro_internal_lookup(noObj.data(), noObj.size(), at) == 0, "nn::ro: без ldr x0,[x1] — образец не узнан");
    CHECK(ro_internal_lookup(nullptr, 16, at) == 0, "nn::ro: нет кода — 0");
    const uint64_t back = 0x1000;   // bl назад (отрицательное смещение)
    const std::vector<uint32_t> lmsBack = {LdrX0X1, MovX1X2, Bl(0x2000 + 2 * 4, back)};
    CHECK(ro_internal_lookup(lmsBack.data(), lmsBack.size(), 0x2000) == back, "nn::ro: bl назад — знак смещения");
    const std::vector<uint32_t> fOld = {Stp80, StpX20, MovX29Sp, LdrX8At80, MovX19X1};
    const std::vector<uint32_t> fNew = {Stp80, StpX20, MovX29Sp, LdrbW9At195, MovX19X1, LdrX8At80};
    const std::vector<uint32_t> fOther = {Stp80, MovX29Sp, MovX19X1, MovX20X1};
    CHECK(ro_lookup_sysv_only(fOld.data(), fOld.size()), "nn::ro: ldr [x0,#80] без флага — старая (только DT_HASH)");
    CHECK(!ro_lookup_sysv_only(fNew.data(), fNew.size()), "nn::ro: сначала флаг [x0,#195] — знает DT_GNU_HASH");
    CHECK(!ro_lookup_sysv_only(fOther.data(), fOther.size()), "nn::ro: непохожий код — не старая (модуль не трогает)");
    CHECK(ro_lookup_kind(NsoImage(make_nso({Stp32, LdrX0X1, MovX1X2, Bl(0x100 + 3 * 4, 0x140)}, fOther,
                                           kRoLookupModuleSymbol, false))) == "other",
          "nn::ro по NSO: F без флага и без obj+80 -> other (не старая, но и не «знает GNU»)");

    // Тот же разбор по файлу NSO: экспорт по 0x100, F по 0x140.
    const std::vector<uint32_t> lmsF = {Stp32, LdrX0X1, MovX1X2, Bl(0x100 + 3 * 4, 0x140)};
    const std::string LMS = kRoLookupModuleSymbol;
    CHECK(ro_lookup_kind(NsoImage(make_nso(lmsF, fOld, LMS, false))) == "sysv_only", "nn::ro по NSO: старая F -> sysv_only");
    CHECK(ro_lookup_kind(NsoImage(make_nso(lmsF, fNew, LMS, false))) == "gnu", "nn::ro по NSO: новая F -> gnu");
    auto lmsBad = lmsF;
    lmsBad[2] = MovX20X1;
    CHECK(ro_lookup_kind(NsoImage(make_nso(lmsBad, fOld, LMS, false))) == "unknown", "nn::ro по NSO: образец не узнан -> unknown");
    CHECK(ro_lookup_kind(NsoImage(make_nso(lmsF, fOld, "_ZN2nn2ro6OtherEv", false))) == "absent",
          "nn::ro по NSO: нет LookupModuleSymbol -> absent");

    // forecast_game: DT_GNU_HASH у модулей игры; forecast_sdk + forecast_ro_fix — вердикт.
    char tmpl[] = "/tmp/mk-unit-rofix-XXXXXX";
    std::string root = mkdtemp(tmpl);   // временное — остаётся (ничего не удаляем)
    mkhost::StdFs sfs;
    mkhost::Clock clk;
    Env env{sfs, soft_hash(), clk};
    auto put = [&](const std::string& p, const Bytes& b) {
        sfs.make_dirs(path_parent(p));
        sfs.write_file_replace(p, b.data(), b.size());
    };
    put(root + "/exefs/main", make_nso({}, {}, "game_main_fn", true));
    put(root + "/exefs/subsdk0", make_nso({}, {}, "game_sub_fn", false));
    put(root + "/sdk19", make_nso(lmsF, fOld, LMS, false));
    put(root + "/sdk20", make_nso(lmsF, fNew, LMS, false));
    const ForecastGame g = forecast_game(env, root + "/exefs");
    CHECK(g.modules == std::vector<std::string>({"main", "subsdk0"}) && g.gnu_hash_modules == std::vector<std::string>({"main"}),
          "nn::ro: модули игры прочитаны, DT_GNU_HASH — только у main");
    const ForecastSdk s19 = forecast_sdk(env, root + "/sdk19"), s20 = forecast_sdk(env, root + "/sdk20");
    RoFixForecast r = forecast_ro_fix(g, s19);
    CHECK(s19.ro_lookup == "sysv_only" && r.verdict == "replace_gnu" && r.gnu_modules == std::vector<std::string>({"main"}) &&
              r.modules == 2,
          "nn::ro: старая sdk + DT_GNU_HASH у игры -> модуль подменит поиск");
    CHECK(forecast_ro_fix(g, s20).verdict == "not_needed", "nn::ro: sdk знает DT_GNU_HASH -> не нужно");
    ForecastGame noGnu = g;
    noGnu.gnu_hash_modules.clear();
    CHECK(forecast_ro_fix(noGnu, s19).verdict == "replace", "nn::ro: старая sdk, у игры только DT_HASH -> всё равно подменит");
    ForecastSdk sx = s19;
    sx.ro_lookup = "other";
    CHECK(forecast_ro_fix(g, sx).verdict == "not_old", "nn::ro: F другого вида -> модуль не трогает");
    sx.ro_lookup = "absent";
    CHECK(forecast_ro_fix(g, sx).verdict == "no_ro", "nn::ro: нет nn::ro -> нечего подменять");
    sx.ro_lookup = "unknown";
    CHECK(forecast_ro_fix(g, sx).verdict == "unknown", "nn::ro: код не узнан -> unknown");
    sx.ro_lookup.clear();
    CHECK(forecast_ro_fix(g, sx).verdict.empty(), "nn::ro: не считалось (подставные списки) -> строки нет");
}

int main() {
    test_ro_lookup();
    test_sha();
    test_bic_table();
    test_zstd_frames();
    test_lz4();
    test_npdm();
    test_receipt();
    test_inflate_nacp();
    test_names_versions();
    test_rules();
    test_forecast_parts();
    test_sdk_gaps();
    test_executor();
    test_own_no_bak();
    test_logs();
    test_harvest();
    std::printf("unit: прошло %d, провалено %d\n", g_pass, g_fail);
    if (g_pass + g_fail < 80) {
        std::printf("ЗАТВОР НУЛЯ: проверок меньше 80\n");
        return 2;
    }
    return g_fail ? 1 : 0;
}
