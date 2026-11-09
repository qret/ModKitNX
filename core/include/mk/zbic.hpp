// ZBIC: распознавание (бит 7 флагов NSO0) и снятие — несжатый NSO0 как у эталона.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Распаковка: upstream zstd 1.5.7 (BSD) + наш разбор таблиц частот FSE в binary
// interpolative coding (порт read_ncount_bic из nxredirect/scripts/zbic_pure.py;
// алгоритм выверен там по FSE_readNCount_bic из zstd.c Atmosphere-NX).
// Сборка несжатого NSO — порт zbic_nso.rebuild, гейт — порт zbic_nso.check (13 проверок).
#pragma once

#include "mk/core.hpp"
#include "mk/nso.hpp"

namespace mk {

constexpr int kMinZbicChecks = 13;

// Распаковать все кадры (ZBIC или обычный zstd) подряд. expected — ожидаемый размер
// (из заголовка NSO): результат обязан совпасть. Бросает Error("ZBIC_DECODE", ...).
Bytes zbic_decompress(const uint8_t* src, size_t n, size_t expected);

struct ZbicRebuildInfo {
    uint64_t size_before = 0, size_after = 0;
    uint32_t flags_before = 0, flags_after = kNsoFlagsUncompressed;
    std::string module_id;          // hex
    std::string src_sha256, dst_sha256;
};

// Несжатый NSO0 из ZBIC-NSO (побайтно как zbic_nso.rebuild): шапка 0x100 из оригинала,
// имя модуля сразу за шапкой, flags 0x38, сегменты подряд; каждый сегмент сверяется с
// sha256 из заголовка. Бросает Error("ZBIC_NOT_ZBIC" | "ZBIC_DECODE" | "ZBIC_HASH" ...).
Bytes zbic_rebuild(const Bytes& src_nso, IHash& hash, ZbicRebuildInfo* info = nullptr);

struct GateResult {
    std::vector<std::string> problems;
    int checks = 0;
    bool ok(int min_checks) const { return problems.empty() && checks >= min_checks; }
};
// Гейт пересобранного NSO против оригинала (порт zbic_nso.check, 13 проверок).
GateResult zbic_check(const Bytes& rebuilt, const Bytes& orig, IHash& hash);

struct ZbicScan {
    bool ok = false;
    std::vector<std::string> nso_files, not_nso, zbic_files;   // по имени, отсортированы
    bool zbic() const { return !zbic_files.empty(); }
};
// Осмотр папки exefs ПО СОДЕРЖИМОМУ: заголовок каждого файла (zbic_nso.scan).
ZbicScan zbic_scan(IFileSystem& fs, const std::string& exefs);

// Нужно ли снимать ZBIC (только когда старая Atmosphère его не понимает). Источник правила: ZBIC появился в
// HOS 22.0.0; Atmosphère распаковывает его с 1.12.0; AMS младше 1.12.0 HOS 22 не загружает
// (матрица совместимости HOS × Atmosphère). Значит:
//   нет ZBIC в exefs                       -> "not_compressed"
//   HOS >= 22.0.0 (тогда AMS >= 1.12.0)    -> "not_needed_hos22"
//   HOS < 22 (или неизвестна), AMS >= 1.12 -> "not_needed_ams112"
//   иначе (AMS < 1.12.0 или неизвестна)    -> "needed"
struct ZbicNeed {
    std::string verdict;    // см. выше
    std::string text_key;   // i18n "core/zbic/<verdict>"
};
ZbicNeed zbic_need(const std::array<int, 3>* hos, const std::array<int, 3>* ams, bool zbic_in_exefs);

}  // namespace mk
