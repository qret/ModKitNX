// Установка / снятие ZBIC / осмотр / снятие по квитанции (по эталонной реализации; сверка — tests/host/compare_pc.py).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mk/core.hpp"
#include "mk/mod.hpp"
#include "mk/plan.hpp"
#include "mk/receipt.hpp"
#include "mk/zbic.hpp"

#include <map>
#include <optional>

namespace mk {

// classify_nso: "ours" | "old-sd-mod" | "alien" | "not-nso" + пояснение (RU).
std::pair<std::string, std::string> classify_nso(const Bytes& raw);

// Слоты subsdk0..9, занятые самой игрой (по файлам exefs игры).
std::vector<std::string> occupied_slots(IFileSystem& fs, const std::string& game_exefs);

struct ModuleSource {
    std::string path;            // romfs:/modules/autofix/subsdk (консоль) или файл на ПК
    std::string expect_sha256;   // из manifest.json; пусто — не проверять
};

struct InstallOptions {
    std::string exefs;            // exefs игры (на консоли — смонтированная Code FS)
    std::string dest;             // корень назначения ("sdmc:/" или папка на ПК)
    std::string tid;              // пусто — из main.npdm игры
    ModuleSource module;          // основной модуль (autofix)
    ModuleSource module_early;    // модуль с ранним перехватом (autofix-earlyfs) — для мода со своим модулем
    std::string slot;             // принудительно (subsdkN); пусто — выбор по правилу эталона
    std::string mod;              // мод целиком (папка); пусто — нет
    std::string mod_files;        // просто файлы мода (папка) -> files/
    std::string swap_sdk;         // подменить sdk игры этим nnSdk (exefs/sdk, как --swap-sdk эталона)
    bool zbic_fix = false;        // снять ZBIC вместе с установкой
    bool swapped_sdk = false;     // комплект для подменённой sdk (zbic_fix мягкий)
    bool no_autofix = false;      // compat = []
    bool allow_other_mods = false;
    bool diag = false;            // ставится отладочный вариант модуля (поле квитанции diag, как --diag эталона)
    bool diag_gfx = false;        // в модуле разбор картинки (поле квитанции diag_gfx, как --diag-gfx эталона)
    std::string installed_by = "console";
    std::string pipeline_version = kPipelineVersion;
};

struct InstallPlan : Plan {
    std::string tid, slot, npdm_mode = "game";
    bool updating = false, early = false;
    ZbicScan zbic;
    std::optional<ModPlan> mod;
    std::vector<std::string> busy_game, keeping, replacing_foreign;
    std::string module_used;
    std::string module_variant;   // вид сборки ставящегося модуля (поле квитанции module_variant)
    // Что стояло до этой установки (по прежней квитанции): модуль был (prev_known), его вид и отладочный ли он.
    bool prev_known = false, prev_diag = false;
    std::string prev_variant;
};

// Вид сборки модуля одним словом — module_variant эталона (id варианта поставки): autofix, autofix-earlyfs,
// plain, diag, diag-gfx, plain-gfx, autofix-diag, autofix-diag-gfx, autofix-diag-earlyfs.
std::string module_variant(bool autofix, bool diag, bool diag_gfx, bool early);

// Полная установка (мод + модуль + npdm [+ ZBIC]). Ничего не пишет.
InstallPlan plan_install(Env& env, const InstallOptions& opt);
// Только снять ZBIC (--zbic-only). Ничего не пишет.
InstallPlan plan_zbic_only(Env& env, const InstallOptions& opt);

// Что стоит в назначении по игре (для экрана «Игра» и перед снятием).
struct InstalledState {
    std::string tid, title_dir;
    bool dir_exists = false;
    ReceiptRead receipt;
    std::map<std::string, std::pair<std::string, std::string>> slots;   // subsdkN -> (класс, пояснение)
    bool has_npdm_orig = false, has_romfs = false;
    std::vector<std::string> deferred;          // *.bak-*, *.removed-* в exefs
    std::optional<bool> module_matches;         // sha256 модуля == квитанции
};
InstalledState inspect_installed(Env& env, const std::string& dest, const std::string& tid);

// «Игра обновилась после установки»: ModuleId каждого нашего несжатого NSO (zbic_files
// квитанции) против того же файла exefs игры. false — устарело (переустановить), true — совпало,
// nullopt — сравнивать нечего. stale — имена разошедшихся файлов. Читаются только заголовки.
std::optional<bool> installed_matches_game(Env& env, const std::string& dest, const std::string& tid,
                                           const std::string& game_exefs, std::vector<std::string>* stale = nullptr);

// Снятие по квитанции (настоящее удаление):
//  - удаляется только то, что названо в квитанции И чей sha256 совпал с записанным
//    (written / module_sha256 / npdm_sha256); изменённый после установки — не трогается;
//  - main.npdm: наш удаляется, main.npdm.orig переименовывается обратно в main.npdm;
//  - чужой файл, отложенный нами в .bak-… при установке (backups), возвращается на место;
//  - пустые папки, созданные нами (created_dirs), убираются; квитанция удаляется последней.
// plan.lines — «что будет сделано», plan.warnings — «что не трогаем и почему» (до подтверждения).
Plan plan_uninstall(Env& env, const std::string& dest, const std::string& tid, bool also_files = true);

std::string title_dir_of(const std::string& dest, const std::string& tid);   // <dest>/atmosphere/contents/<TID>

}  // namespace mk
