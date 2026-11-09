// ModKitNX core — разбор NCA3 (заголовок уже расшифрован), секция exefs (PFS0), AES-CTR секции, билет ES.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Зачем: на HOS 19.0.1 fsp-srv не открывает Code FS игры (проверено на консоли: 2002-6001 InvalidArgument у
// OpenFileSystemWithId и OpenFileSystemWithPatch с типом Code — Code открывает только загрузчик через fsp-ldr).
// Путь 3 — читать exefs из сырого NCA самим. Здесь — переносимая часть (без libnx): что где лежит в NCA,
// какой ключ нужен, как считается счётчик CTR, как разобрать PFS0. Платформа даёт два действия:
//   RawReadFn — прочитать байты зашифрованного NCA (ncm ReadContentIdFile);
//   CtrFn     — расшифровать на месте блоки AES-CTR (libnx aes128Ctr с ключом из key area или spl ComputeCtr
//               со слотом титульного ключа).
// Формат — как у hactool / nxdumptool (nca.c, pfs.c, tik.h) и Atmosphère; код свой.
// Ядро ключей не хранит и не печатает: в ошибки и журнал попадают только номера, размеры и смещения.
#pragma once

#include "mk/core.hpp"

#include <array>
#include <functional>

namespace mk::nca {

constexpr size_t kHeaderSize = 0xC00;
constexpr uint64_t kMediaUnit = 0x200;

enum : uint8_t { kContentProgram = 0 };
enum : uint8_t { kFsRomFs = 0, kFsPartition = 1 };
enum : uint8_t { kHashAuto = 0, kHashNone = 1, kHashSha256 = 2, kHashIntegrity = 3 };
enum : uint8_t { kEncAuto = 0, kEncNone = 1, kEncXts = 2, kEncCtr = 3, kEncCtrEx = 4, kEncCtrSkipHash = 5, kEncCtrExSkipHash = 6 };
// Индекс ключа в key area для AES-CTR (0, 1 — XTS, 2 — CTR, 3 — не используется).
constexpr int kKeyAreaCtr = 2;

struct FsSection {
    bool present = false;
    int index = 0;
    uint64_t offset = 0, size = 0;          // в байтах от начала NCA
    uint8_t fs_type = 0, hash_type = 0, encryption_type = 0;
    std::array<uint8_t, 8> ctr_upper{};      // старшие 8 байт счётчика (уже в порядке big-endian)
    uint64_t pfs0_offset = 0, pfs0_size = 0; // для PFS0 со списком хешей SHA-256: от начала секции
    bool has_sparse = false, has_compression = false;
};

struct Header {
    uint8_t content_type = 0;       // @0x205
    uint8_t key_generation_old = 0; // @0x206
    uint8_t kaek_index = 0;         // @0x207: 0 application, 1 ocean, 2 system
    uint64_t content_size = 0;      // @0x208
    uint64_t program_id = 0;        // @0x210
    uint32_t sdk_addon = 0;         // @0x21C
    uint8_t key_generation = 0;     // @0x220
    std::array<uint8_t, 16> rights_id{};  // @0x230
    bool has_rights_id = false;
    std::array<std::array<uint8_t, 16>, 4> key_area{};  // @0x300, зашифровано KAEK
    std::array<FsSection, 4> sections{};
};

// Поколение ключа для spl (GenerateAesKek / PrepareCommonEsTitleKey): max(@0x206, @0x220). Значения 0 и 1 —
// мастер-ключ 0 (secmon сам вычитает 1).
inline uint8_t effective_key_generation(const Header& h) {
    return h.key_generation > h.key_generation_old ? h.key_generation : h.key_generation_old;
}

// Разбор расшифрованного заголовка (0xC00 байт). Проверяет магию NCA3, хеши заголовков секций (SHA-256 @0x280).
// Ошибки: NCA_HEADER (магия/размер), NCA_FS_HASH (хеш заголовка секции не сошёлся — ключ заголовка не тот или
// повреждение).
Header parse_header(const uint8_t* dec, size_t n, IHash& hash);

// Секция exefs program NCA: тип контента Program, секция 0, PFS0 с хешами SHA-256, AES-CTR без BKTR.
// Ошибки: NCA_NOT_PROGRAM, NCA_NO_EXEFS, NCA_EXEFS_CRYPTO (тип шифрования не CTR/None — номер в args).
const FsSection& exefs_section(const Header& h);

// Счётчик AES-CTR для абсолютного смещения abs_offset в NCA (кратно 16): ctr_upper || BE64(abs_offset >> 4).
std::array<uint8_t, 16> ctr_for(const FsSection& s, uint64_t abs_offset);

// Программная расшифровка записи key area (для хост-тестов и пути с prod.keys): AES-ECB.
std::array<uint8_t, 16> decrypt_key_area_entry(const Header& h, int index, const uint8_t kaek[16]);

using RawReadFn = std::function<void(uint64_t offset, uint8_t* out, size_t n)>;       // бросает Error
using CtrFn = std::function<void(const uint8_t ctr[16], uint8_t* data, size_t n)>;    // n кратно 16; на месте

// CtrFn из открытого ключа (переносимый AES ядра; на консоли платформа даёт свой, быстрый).
CtrFn soft_ctr(const uint8_t key[16]);

// Чтение расшифрованных байт секции по смещению от начала секции (любые смещения и длины).
class SectionReader {
public:
    SectionReader(RawReadFn raw, CtrFn ctr, const FsSection& s);
    void read(uint64_t rel, uint8_t* out, size_t n) const;   // за концом секции — Error NCA_RANGE
    const FsSection& section() const { return s_; }
private:
    RawReadFn raw_;
    CtrFn ctr_;
    FsSection s_;
};

// ---------------- PFS0
struct Pfs0Entry {
    std::string name;
    uint64_t offset = 0;  // от начала данных PFS0 (т. е. от pfs0_offset + header_size)
    uint64_t size = 0;
};
struct Pfs0 {
    uint64_t header_size = 0;
    std::vector<Pfs0Entry> entries;
    const Pfs0Entry* find(const std::string& name) const;
};
// Размер заголовка по первым 0x10 байтам. Ошибка NCA_PFS0_MAGIC — не «PFS0» (обычно — не тот ключ).
uint64_t pfs0_header_size(const uint8_t* first16);
// Разбор заголовка PFS0 целиком (header_size байт). Проверяет имена и границы (data_size — размер данных).
Pfs0 parse_pfs0(const uint8_t* hdr, size_t n, uint64_t data_size);

// Exefs поверх SectionReader: заголовок PFS0 читается при open.
class ExefsReader {
public:
    explicit ExefsReader(const SectionReader& r);
    const Pfs0& pfs0() const { return pfs_; }
    void read(const Pfs0Entry& e, uint64_t off, uint8_t* out, size_t n) const;   // за концом файла — Error
private:
    const SectionReader& r_;
    Pfs0 pfs_;
};

// ---------------- билет ES (тикет)
struct Ticket {
    uint32_t sig_type = 0;
    uint8_t titlekey_type = 0;     // 0 common, 1 personalized
    uint8_t key_generation = 0;    // поколение мастер-ключа (кодировка как в NCA)
    std::array<uint8_t, 16> rights_id{};
    std::array<uint8_t, 16> enc_titlekey{};   // для common — первые 16 байт titlekey_block
};
// Ошибка TICKET_FORMAT — неизвестный тип подписи или короткий буфер.
Ticket parse_ticket(const uint8_t* p, size_t n);

std::string rights_id_hex(const std::array<uint8_t, 16>& r);

}  // namespace mk::nca
