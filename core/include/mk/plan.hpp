// «Сначала план, потом запись»: функции ядра возвращают Plan, исполнитель пишет через IFileSystem.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mk/core.hpp"

#include <functional>
#include <map>
#include <nlohmann/json.hpp>

namespace mk {

enum class ActKind {
    MakeDir,      // dst (новые папки внутри назначения запоминаются: created_dirs)
    PutFile,      // src -> dst (копия). Есть и такое же — пропуск; иное СВОЁ — перезапись начисто (без .bak);
                  // иное ЧУЖОЕ — сначала копия dst.bak-<stamp> (foreign_backups)
    PutBytes,     // data -> dst (то же правило, что PutFile)
    SaveOrig,     // src -> dst, только если src есть, а dst нет (main.npdm -> main.npdm.orig); dst — наш
    PutReceipt,   // receipt -> dst: "installed" и учёт owned/own_backups/foreign_backups/created_dirs — исполнитель
    Delete,       // dst удаляется, ТОЛЬКО если sha256 == expect_sha256 (иначе — находка, файл цел)
    Rename,       // src -> dst (main.npdm.orig -> main.npdm; чужой .bak-… -> на своё место)
    RemoveEmptyDir,  // dst, если пуст (папки, созданные нами); не пуст — пропуск без ошибки
};

struct Action {
    ActKind kind = ActKind::MakeDir;
    std::string src, dst;
    std::shared_ptr<const Bytes> data;   // PutBytes
    nlohmann::ordered_json receipt;      // PutReceipt
    std::string what;     // "module" | "npdm" | "zbic-nso" | "mod-exefs" | "mod-patch" | "mod-extra" |
                          // "mod-file" | "npdm-orig" | "receipt" | "sdk" | "restore" | "dir"
    bool replaces_foreign = false;       // кладём поверх файла, который клали не мы (для плана и UI)
    std::string expect_sha256;           // Put*: что должно лечь; Delete: что должно лежать
    std::string rel;                     // путь от корня назначения
    // Учёт своего как Ledger.put эталона: прежний файл на месте считается нашим, если его путь
    // в owned прежней квитанции, или имя — в ours_names, или assume_ours.
    std::vector<std::string> ours_names;
    bool assume_ours = false;
    // Непусто — dst считается своим, если этот путь наш (owned этого прогона или прежней квитанции):
    // main.npdm при сохранённом нами main.npdm.orig (эталон: assume_ours=(dst == dest_npdm and orig_ours)).
    std::string ours_if_owned;
    bool sdk_swap = false;              // exefs/sdk подмены: копия чужого — в sdk_swap.foreign_backup
    // PutReceipt: прежняя квитанция (owned/own_backups/foreign_backups/created_dirs сливаются с ней)
    nlohmann::ordered_json receipt_base;
    std::string installed_by;            // непусто — дописать поле installed_by после полей формата
};

struct Plan {
    std::string stamp;                     // суффикс .bak- (IClock::stamp на момент плана)
    std::string dest;                      // корень назначения
    std::vector<Action> actions;
    Texts lines;        // «что будет сделано» человеческими строками (RU) + ключи перевода (Texts::msg)
    Texts warnings;     // предупреждения и «не трогаем, потому что…» (RU) + ключи перевода
    // Гейт сохранности чужого: папка и {имя: sha256} чужих файлов ДО записи.
    std::string guard_dir;
    std::map<std::string, std::string> foreign_before;
};

struct ApplyReport {
    Texts done;        // что сделано
    Texts skipped;     // что не тронуто и почему (Delete с чужим sha и т. п.)
    Texts problems;    // гейты после записи (пусто — всё сошлось)
    nlohmann::ordered_json receipt;       // записанная квитанция (если была)
};

// Исполнитель (порт Ledger и write_receipt эталона). После записи: каждый положенный файл
// сверяется по sha256 с ожидаемым,
// чужие файлы guard_dir — с foreign_before. Находка гейта — Error("FAIL_GATE_APPLY").
ApplyReport apply_plan(Env& env, const Plan& plan);
// То же с прогрессом: progress(i, n, действие) перед каждым действием (i от 0).
using ProgressFn = std::function<void(size_t, size_t, const Action&)>;
ApplyReport apply_plan(Env& env, const Plan& plan, const ProgressFn& progress);

// {имя: sha256} файлов папки (без раскрытия подпапок) — snapshot_dir эталона.
std::map<std::string, std::string> snapshot_sha(Env& env, const std::string& dir);

}  // namespace mk
