// Квитанция nxredirect.json — формат 1 (write_receipt эталона) + installed_by.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mk/core.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <set>

namespace mk {

using ojson = nlohmann::ordered_json;

// Прочитанная квитанция: нет файла / битая / объект.
struct ReceiptRead {
    bool present = false;
    bool broken = false;
    std::string broken_why;
    ojson data;               // объект (если present && !broken)
    bool sane() const { return present && !broken && data.is_object() && !data.empty(); }
};
ReceiptRead read_receipt(Env& env, const std::string& title_dir);   // <dest>/atmosphere/contents/<TID>
ReceiptRead parse_receipt_text(const std::string& text);

// Поля одной записи (пустое — взять из прежней квитанции, как в квитанции формата 1).
struct ReceiptFields {
    std::string tid;
    std::optional<std::string> slot, module_sha256, npdm_sha256, module_name;
    std::vector<std::string> zbic_files, neighbours;
    std::string mode = "install";                 // "install" | "zbic-only" | "sdk-swap"
    std::optional<std::vector<std::string>> compat;
    std::optional<ojson> mod;                     // объект mod или отсутствует (берётся из base)
    std::string version = kPipelineVersion;
    std::string installed_by = "console";         // "" — поле не пишется (для побайтной сверки с эталоном)
    std::optional<ojson> sdk_swap;                // поле формата; нет — берётся из base (или null)
    std::optional<bool> diag;                     // поле формата; нет — из base (или false)
    std::optional<bool> diag_gfx;                 // поле формата 1: разбор картинки; нет — из base (или false)
    std::optional<std::string> module_variant;    // поле формата 1: id варианта модуля; нет — из base (или null)
};

// Квитанция в порядке ключей формата 1 (как write_receipt эталона): schema … mod, sdk_swap,
// diag, diag_gfx, module_variant. "installed" — пустая строка до записи. Исполнитель (apply_plan)
// дописывает учёт своего как Ledger эталона: owned {путь от корня: sha256}, own_backups (НЕ
// пополняется — свои файлы перезаписываются без .bak; несёт только записи прежних квитанций, чтобы
// снятие удалило старые копии), foreign_backups {путь: копия .bak- ЧУЖОГО файла}, created_dirs [папки], — и в конце
// наше поле installed_by ("console"). sdk_swap — формат 1: file, module, version, sha256, size,
// source_name, foreign_backup, foreign_sha256, installed, verified_on_console.
ojson make_receipt(const ReceiptFields& f, const ReceiptRead& base);
// Текст как json.dumps(ensure_ascii=False, indent=2) + "\n".
std::string dump_receipt(const ojson& r);

// exefs_ours_known: имена файлов exefs, которые положили мы (по квитанции).
std::set<std::string> exefs_ours_known(const ReceiptRead& r);

}  // namespace mk
