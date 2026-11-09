// NSO0: заголовок, сегменты (LZ4 / ZBIC / без сжатия), образ, .dynamic, .dynsym.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Порт: zbic_nso.py read_header, check_nso.py Nso (nxredirect/scripts), nso_min.py NSO
// (nxredirect/hosttest). Два способа счёта символов сохранены как есть, потому что от
// них зависят разные решения эталона: classify_nso (check_nso: DT_HASH или до .dynstr) и
// stubdiff (nso_min: GNU hash или (strtab - symtab) / syment).
#pragma once

#include "mk/core.hpp"

#include <map>
#include <optional>

namespace mk {

constexpr uint32_t kNsoFlagZbic = 0x80;          // Flag_UseZbicCompression (бит 7)
constexpr uint32_t kNsoFlagsUncompressed = 0x38; // сжатия нет, три хеша проверяются
constexpr size_t kNsoHeaderSize = 0x100;

struct NsoSegment {
    uint32_t file_offset = 0, memory_offset = 0, size = 0, compressed_size = 0;
    bool compressed = false, hashed = false;
    Digest hash{};
};

struct NsoHeader {
    uint32_t flags = 0;
    NsoSegment seg[3];
    Digest module_id{};                 // 0x40..0x60 (build id)
    uint32_t bss_size = 0;
    uint32_t module_name_offset = 0, module_name_size = 0;
    bool zbic() const { return (flags & kNsoFlagZbic) != 0; }
    std::string module_id_hex(bool upper = false) const { return hex(module_id.data(), 32, upper); }
};

// false — не NSO0 (короче 0x100 или нет магии): для распознавания это ответ, а не ошибка.
bool parse_nso_header(const uint8_t* data, size_t size, NsoHeader& out);

// LZ4-блок с проверкой границ (upstream lz4, BSD-2). Бросает Error("NSO_LZ4").
Bytes lz4_block_decompress(const uint8_t* src, size_t n, size_t expected);

// Сегмент n в распакованном виде (по биту сжатия; ZBIC — по биту 7). Размер обязан
// совпасть с заголовком. verify_hash — сверить с sha256 из заголовка (если сегмент hashed).
Bytes nso_segment(const Bytes& raw, const NsoHeader& h, int n, IHash* verify_hash = nullptr);

struct NsoSymbol {
    std::string name;
    uint8_t info = 0;     // bind << 4 | type
    uint16_t shndx = 0;   // 0 — импорт
    uint64_t value = 0, size = 0;
};

// Разобранный NSO (как nso_min.NSO + check_nso.Nso).
class NsoImage {
public:
    // Бросает Error("NSO_BAD", ...) на негодном файле.
    explicit NsoImage(const Bytes& raw, IHash* verify_hash = nullptr);

    NsoHeader header;
    Bytes text, ro, data;
    Bytes flat;   // плоский образ: сегменты по memory_offset, дыры нулями (check_nso.image)

    // Чтение по адресу как nso_min.mem (вне сегментов — нули).
    Bytes mem(uint64_t addr, size_t n) const;
    // .dynamic
    std::map<int64_t, std::vector<uint64_t>> dt;
    // Символы по способу nso_min (stubdiff).
    std::vector<NsoSymbol> syms_min() const;
    std::map<std::string, NsoSymbol> exports() const;   // shndx != 0, имя непустое
    std::map<std::string, NsoSymbol> imports() const;   // shndx == 0

    std::optional<std::string> module_name() const;      // stubdiff.module_name
    // Наибольшая "SDK MW+Nintendo+NintendoSDK_<lib>-X_Y_Z" в последних 0x4000 rodata.
    std::optional<std::array<int, 3>> mw_version() const;
    std::string build_id_upper() const { return header.module_id_hex(true); }

private:
    uint32_t mod_off_ = 0;
    uint64_t dyn_addr_ = 0;
    std::vector<uint64_t> dyn_order_;
};

// Разбор как check_nso.Nso (им пользуются classify_nso и mod_module_info эталона): плоский образ,
// MOD0, .dynamic из образа (теги — последний выигрывает, DT_NEEDED отдельно).
struct NsoFlat {
    NsoHeader header;
    Bytes image;
    std::map<int64_t, uint64_t> tags;
};
NsoFlat parse_nso_flat(const Bytes& raw);                       // бросает Error("NSO_BAD")
std::vector<std::string> nso_flat_imports(const NsoFlat& n);    // check_nso.symbols(): импорты; бросает

std::string version_text(const std::array<int, 3>& v);   // "19.3.13"

}  // namespace mk
