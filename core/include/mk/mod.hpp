// Мод целиком: разбор папки мода (порт find_mod_tree / analyze_mod / resolve_mod_input эталона).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mk/core.hpp"

#include <map>
#include <optional>

namespace mk {

struct ModTree {
    std::string root;
    std::optional<std::string> atmosphere;          // папка atmosphere мода (или её аналог)
    std::map<std::string, std::string> titles;      // TID (ЗАГЛАВНЫМИ) -> папка игры в моде
};
// Опознать дерево мода: <корень>/atmosphere/contents/<TID>, atmosphere, contents/<TID>, <TID>.
std::optional<ModTree> find_mod_tree(IFileSystem& fs, const std::string& path);

struct ModModuleInfo {
    std::string name, sha256, build_id, problem;
    uint64_t size = 0;
    bool nso = false, hooks_openfile = false, imports_openfile = false, replaces_game = false;
};

struct ModPatch {
    std::string rel, path;                 // путь от atmosphere/exefs_patches и источник
    std::optional<std::string> target;     // "main игры" / "subsdk3 мода" — к чему подходит IPS
};

struct ModPlan {
    std::string root, title_dir;
    std::map<std::string, std::string> exefs;           // имя -> источник (кроме main.npdm)
    std::map<std::string, ModModuleInfo> modules;       // subsdkN мода
    std::vector<std::string> replaces;                  // имена exefs мода, которые есть у игры
    std::optional<std::string> npdm;                    // main.npdm мода
    std::vector<std::pair<std::string, std::string>> files;   // (путь в files/, источник), по пути
    std::vector<ModPatch> patches;
    std::vector<std::pair<std::string, std::string>> extra;   // (путь от папки игры, источник)
    Texts skipped, warnings;
    bool needs_early_module() const { return !modules.empty(); }   // autofix-earlyfs
};

// analyze_mod. Ошибки — коды эталона: FAIL_MOD_NOT_TREE, FAIL_MOD_OTHER_TITLE, FAIL_MOD_ROMFS_BIN,
// FAIL_MOD_FILES_CLASH, FAIL_MOD_EMPTY.
ModPlan analyze_mod(Env& env, const std::string& mod_path, const std::string& tid,
                    const std::string* game_exefs);

// mod_slot_order: (хорошо_ли, пояснение) — наш слот старше модулей мода? msg — то же пояснение ключом.
std::pair<bool, std::string> mod_slot_order(const std::string& slot, const ModPlan& plan, Msg* msg = nullptr);

int slot_number(const std::string& name);   // subsdkN -> N, иначе -1

}  // namespace mk
