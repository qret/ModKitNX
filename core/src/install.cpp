// Установка: порт эталона (main, choose_slot, do_zbic_only, classify_nso, occupied_slots;
// сверка — tests/host/compare_pc.py); осмотр и снятие по квитанции (настоящее удаление).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/install.hpp"
#include "mk/npdm.hpp"
#include "mk/sdk.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <set>

namespace mk {

namespace {

bool contains(const Bytes& hay, const char* needle) {
    size_t n = std::strlen(needle);
    if (hay.size() < n) return false;
    return std::search(hay.begin(), hay.end(), needle, needle + n) != hay.end();
}

std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}
template <class C> std::vector<std::string> sorted(const C& c) {
    std::vector<std::string> v(c.begin(), c.end());
    std::sort(v.begin(), v.end());
    return v;
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

std::map<std::string, std::pair<std::string, std::string>> scan_dest(Env& env, const std::string& dest_exefs) {
    std::map<std::string, std::pair<std::string, std::string>> found;
    if (!env.fs.is_dir(dest_exefs)) return found;
    for (int n = 0; n < 10; n++) {
        std::string nm = "subsdk" + std::to_string(n), p = path_join(dest_exefs, nm);
        if (env.fs.is_file(p)) found[nm] = classify_nso(env.fs.read_file(p));
    }
    return found;
}

const char* label_key(const std::string& cls) {
    if (cls == "ours") return "plan/slot_ours";
    if (cls == "old-sd-mod") return "plan/slot_old_sd_mod";
    if (cls == "alien") return "plan/slot_alien";
    return "plan/slot_not_nso";
}

const char* label_of(const std::string& cls) {
    if (cls == "ours") return "наш nxredirect";
    if (cls == "old-sd-mod") return "ЧУЖОЙ мод, читающий SD";
    if (cls == "alien") return "чужой модуль";
    return "не NSO";
}

void rglob(IFileSystem& fs, const std::string& d, const std::string& rel,
           std::vector<std::pair<std::string, std::string>>& out) {
    for (auto& e : fs.list_dir(d)) {
        std::string p = path_join(d, e.name), r = rel.empty() ? e.name : rel + "/" + e.name;
        if (e.is_dir) rglob(fs, p, r, out);
        else out.emplace_back(r, p);
    }
}

uint64_t npdm_pid(Env& env, const std::string& path) { return Npdm(env.fs.read_file(path)).program_id(); }

std::string exefs_problem(IFileSystem& fs, const std::string& p) {
    if (!fs.exists(p)) return "папки нет";
    if (!fs.is_dir(p)) return "это не папка";
    std::vector<std::string> miss;
    for (const char* n : {"main.npdm", "main", "rtld", "sdk"})
        if (!fs.is_file(path_join(p, n))) miss.push_back(n);
    if (!miss.empty()) return "нет обязательных файлов: " + join(miss);
    return "";
}

// choose_slot эталона. Возвращает (слот, обновление).
std::pair<std::string, bool> choose_slot(Env& env, const InstallOptions& opt, const std::vector<std::string>& busy_game,
                                         const std::string& dest_base, const std::string& dest_exefs,
                                         Texts& warnings, Texts& lines,
                                         const ModPlan* mod_plan) {
    ReceiptRead rec = read_receipt(env, dest_base);
    auto found = scan_dest(env, dest_exefs);
    std::set<std::string> mod_slots;
    if (mod_plan)
        for (auto& [k, v] : mod_plan->modules) mod_slots.insert(k);
    if (rec.sane() && rec.data.contains("mod") && rec.data["mod"].is_object() && rec.data["mod"].contains("exefs"))
        for (auto& x : rec.data["mod"]["exefs"])
            if (x.is_string() && slot_number(x.get<std::string>()) >= 0) mod_slots.insert(x.get<std::string>());
    for (auto& s : mod_slots)
        if (found.count(s)) {
            lines.add(s + ": модуль МОДА (переносится вместе с модом), не чужой", "plan/slot_mod_module", {s});
            found.erase(s);
        }
    std::vector<std::string> ours, old_sd, broken;
    for (auto& [s, cw] : found) {
        if (cw.first == "ours") ours.push_back(s);
        if (cw.first == "old-sd-mod") old_sd.push_back(s);
        if (cw.first == "not-nso") broken.push_back(s);
        lines.add(s + ": " + label_of(cw.first) + " — " + cw.second, label_key(cw.first), {s});
    }
    if (!old_sd.empty() && !opt.allow_other_mods)
        throw Error("FAIL_FOREIGN_SD_MOD", "в назначении уже стоит ЧУЖОЙ мод, который сам читает SD-карту: " +
                                               join(old_sd) + ". Сначала снимите старый мод вручную.", {join(old_sd)});
    if (!broken.empty())
        warnings.add("в назначении лежит файл, который не разбирается как NSO: " + join(broken) +
                         ". Loader грузит все subsdk0..subsdk9 и на таком файле споткнётся — игра не запустится, "
                         "даже если наш модуль встанет в другой слот. Уберите этот файл ВРУЧНУЮ (я ничего не "
                         "удаляю) и проверьте результат командой --status",
                     "warn/not_nso_in_dest", {join(broken)});
    if (!old_sd.empty())
        warnings.add("в назначении остался чужой мод, читающий SD (" + join(old_sd) +
                         "), — вы разрешили это флагом --allow-other-mods; если игра поведёт себя странно, снимите его",
                     "warn/old_sd_mod_allowed", {join(old_sd)});
    std::string rec_slot;
    if (rec.present && rec.broken)
        warnings.add(std::string("квитанция ") + kReceiptName + " не читается (" + rec.broken_why +
                         "); слот определяется по содержимому модулей", "warn/receipt_broken_slot", {kReceiptName});
    else if (rec.present && rec.data.contains("slot") && rec.data["slot"].is_string())
        rec_slot = rec.data["slot"].get<std::string>();

    if (!rec_slot.empty() && mod_slots.count(rec_slot))
        throw Error("FAIL_MOD_SLOT_CONFLICT", "наш модуль по квитанции стоит в " + rec_slot + ", а этот слот нужен модулю мода",
                    {rec_slot});
    if (!opt.slot.empty() && mod_slots.count(opt.slot))
        throw Error("FAIL_MOD_SLOT_CONFLICT", "слот " + opt.slot + " занят модулем мода — наш модуль туда не ляжет", {opt.slot});
    if (!rec_slot.empty()) {
        auto it = found.find(rec_slot);
        if (it != found.end() && it->second.first != "ours")
            throw Error("FAIL_FOREIGN_IN_OUR_SLOT", "в слоте " + rec_slot + ", который занимает наш мод по квитанции, "
                                                    "лежит НЕ наш модуль: " + it->second.second, {rec_slot});
        if (!ours.empty() && std::find(ours.begin(), ours.end(), rec_slot) == ours.end()) {
            if (!opt.slot.empty() && std::find(ours.begin(), ours.end(), opt.slot) != ours.end()) {
                warnings.add("квитанция указывает слот " + rec_slot + ", а наш модуль лежит в " + join(ours) +
                                 "; вы задали --slot " + opt.slot + " — обновляю его и переписываю квитанцию",
                             "warn/receipt_slot_override", {rec_slot, join(ours), opt.slot});
                return {opt.slot, true};
            }
            throw Error("FAIL_RECEIPT_MISMATCH", "квитанция и карта расходятся, ставить второй модуль не буду: наш модуль "
                                                 "опознан в " + join(ours) + ", квитанция указывает " + rec_slot,
                        {join(ours), rec_slot});
        }
        if (!opt.slot.empty() && opt.slot != rec_slot)
            throw Error("FAIL_SLOT_CONFLICT_RECEIPT", "наш модуль уже стоит в слоте " + rec_slot + " (квитанция), а задан "
                                                      "--slot " + opt.slot + ". Сначала снимите прежний.", {rec_slot, opt.slot});
        lines.add("наш модуль уже установлен в " + rec_slot + " (по квитанции) — ОБНОВЛЯЮ этот же слот", "plan/slot_update",
                  {rec_slot});
        return {rec_slot, true};
    }
    if (ours.size() == 1) {
        std::string slot = ours[0];
        warnings.add(std::string("квитанции ") + kReceiptName + " не было, но в " + slot +
                         " опознан наш модуль по содержимому образа — обновляю его и завожу квитанцию",
                     "warn/no_receipt_ours", {slot});
        if (!opt.slot.empty() && opt.slot != slot)
            throw Error("FAIL_SLOT_CONFLICT_CONTENT", "наш модуль уже стоит в слоте " + slot + " (опознан по содержимому), "
                                                      "а задан --slot " + opt.slot, {slot, opt.slot});
        return {slot, true};
    }
    if (ours.size() > 1) throw Error("FAIL_MANY_OURS", "в назначении наших модулей больше одного: " + join(ours), {join(ours)});
    std::set<std::string> busy(busy_game.begin(), busy_game.end());
    for (auto& [s, c] : found) busy.insert(s);
    busy.insert(mod_slots.begin(), mod_slots.end());
    if (!opt.slot.empty()) {
        if (std::find(busy_game.begin(), busy_game.end(), opt.slot) != busy_game.end())
            throw Error("FAIL_SLOT_BUSY_GAME", "слот " + opt.slot + " занят самой игрой — выберите другой", {opt.slot});
        if (found.count(opt.slot))
            throw Error("FAIL_SLOT_OCCUPIED_DEST", "в слоте " + opt.slot + " в назначении лежит чужой файл", {opt.slot});
        return {opt.slot, false};
    }
    for (int n = 9; n >= 0; n--) {
        std::string c = "subsdk" + std::to_string(n);
        if (!busy.count(c)) {
            lines.add("выбран свободный слот " + c + " (старший из свободных)", "plan/slot_free", {c});
            return {c, false};
        }
    }
    throw Error("FAIL_NO_FREE_SLOT", "все слоты subsdk0..subsdk9 заняты — свободного нет");
}

struct Prep {
    std::string exefs, tid, dest_base, dest_exefs;
    uint64_t tid_npdm = 0;
};

Prep prepare(Env& env, const InstallOptions& opt, Texts& warnings) {
    Prep p;
    p.exefs = opt.exefs;
    std::string prob = exefs_problem(env.fs, opt.exefs);
    if (!prob.empty())
        throw Error("FAIL_EXEFS_BAD_ARG", "папка exefs игры не годится: " + opt.exefs + "; причина: " + prob, {opt.exefs});
    p.tid_npdm = npdm_pid(env, path_join(opt.exefs, "main.npdm"));
    std::string tn = tid_hex(p.tid_npdm);
    if (!opt.tid.empty()) {
        std::string t = upper(opt.tid);
        uint64_t dummy;
        if (!parse_tid(t, dummy))
            throw Error("FAIL_BAD_TID", "TitleID должен быть 16 шестнадцатеричными цифрами: " + opt.tid, {opt.tid});
        if (t != tn)
            warnings.add("заданный TitleID " + t + " не совпадает с тем, что в main.npdm игры (" + tn +
                             "); беру заданный — проверьте, что exefs от этой игры", "warn/tid_mismatch", {t, tn});
        p.tid = t;
    } else {
        p.tid = tn;
    }
    p.dest_base = title_dir_of(opt.dest, p.tid);
    p.dest_exefs = path_join(p.dest_base, "exefs");
    return p;
}

// zbic_prepare_exefs: несжатые NSO в памяти, каждый — через гейт 13 проверок.
std::map<std::string, std::shared_ptr<const Bytes>> zbic_prepare(Env& env, const std::string& exefs, const ZbicScan& scan,
                                                                 bool required, Texts& lines) {
    std::map<std::string, std::shared_ptr<const Bytes>> made;
    if (!scan.ok || scan.zbic_files.empty()) {
        if (!required) {
            lines.add("сжатия ZBIC в этой игре нет — несжатый main в комплект не кладётся (это не отказ)", "plan/zbic_none");
            return made;
        }
        throw Error("FAIL_ZBIC_NOT_FOUND", "сжатия ZBIC в " + exefs + " нет — снимать нечего", {exefs});
    }
    for (auto& name : scan.zbic_files) {
        Bytes orig = env.fs.read_file(path_join(exefs, name));
        ZbicRebuildInfo info;
        Bytes out;
        try {
            out = zbic_rebuild(orig, env.hash, &info);
        } catch (const Error& e) {
            throw Error("FAIL_ZBIC_REBUILD", "не удалось собрать несжатый NSO из " + name + ": " + e.what(), {name});
        }
        GateResult g = zbic_check(out, orig, env.hash);
        if (!g.ok(kMinZbicChecks))
            throw Error("FAIL_GATE_ZBIC", "гейт пересобранного NSO не пройден (" + name + "): " +
                                              (g.problems.empty() ? "затвор нуля" : g.problems[0]), {name});
        lines.add(name + ": " + std::to_string(info.size_before) + " Б -> " + std::to_string(info.size_after) +
                      " Б без сжатия (flags 0x38), гейт zbic: " + std::to_string(g.checks) + " из 13",
                  "plan/zbic_unpacked",
                  {name, std::to_string(info.size_before), std::to_string(info.size_after), std::to_string(g.checks)});
        made[name] = std::make_shared<const Bytes>(std::move(out));
    }
    return made;
}

Action put_file(const std::string& src, const std::string& dst, const std::string& what, const std::string& rel,
                const std::string& sha, bool foreign) {
    Action a;
    a.kind = ActKind::PutFile;
    a.src = src;
    a.dst = dst;
    a.what = what;
    a.rel = rel;
    a.expect_sha256 = sha;
    a.replaces_foreign = foreign;
    return a;
}
Action put_bytes(std::shared_ptr<const Bytes> d, const std::string& dst, const std::string& what, const std::string& rel,
                 IHash& h, bool foreign) {
    Action a;
    a.kind = ActKind::PutBytes;
    a.data = d;
    a.dst = dst;
    a.what = what;
    a.rel = rel;
    a.expect_sha256 = sha256_hex(h, *d);
    a.replaces_foreign = foreign;
    return a;
}
Action mkdir_act(const std::string& d) {
    Action a;
    a.kind = ActKind::MakeDir;
    a.dst = d;
    a.what = "dir";
    return a;
}

std::string rel_title(const std::string& tid, const std::string& sub) { return "atmosphere/contents/" + tid + "/" + sub; }

}  // namespace

std::string module_variant(bool autofix, bool diag, bool diag_gfx, bool early) {
    if (autofix)
        return std::string("autofix") + (diag ? "-diag" : "") + (diag_gfx ? "-gfx" : "") + (early ? "-earlyfs" : "");
    return std::string(diag ? "diag" : "plain") + (diag_gfx ? "-gfx" : "") + (early ? "-earlyfs" : "");
}

std::string title_dir_of(const std::string& dest, const std::string& tid) {
    return path_join(path_join(path_join(dest, "atmosphere"), "contents"), upper(tid));
}

std::pair<std::string, std::string> classify_nso(const Bytes& raw) {
    NsoFlat n;
    try {
        n = parse_nso_flat(raw);
    } catch (const std::exception& e) {
        return {"not-nso", std::string("не разобрался как NSO (") + e.what() + ")"};
    }
    std::vector<std::string> imports;
    try {
        imports = nso_flat_imports(n);
    } catch (const std::exception&) {
        imports.clear();
    }
    if (contains(n.image, "sd:/atmosphere/contents/") && contains(n.image, "/files/")) {
        if (!imports.empty())
            return {"alien", "похож на наш путь мода, но у модуля есть импорты (" + std::to_string(imports.size()) +
                                 " шт.) — наш собирается без импортов"};
        return {"ours", "в образе путь мода sd:/atmosphere/contents/<TID>/files/, импортов нет"};
    }
    if (contains(n.image, "sd:/")) {
        std::vector<std::string> marks;
        for (const char* k : {"sd:/tits2rus/", "sd:/atmosphere/"})
            if (contains(n.image, k)) marks.push_back(k);
        return {"old-sd-mod", "модуль сам читает SD" + (marks.empty() ? std::string() : " (" + join(marks) + ")")};
    }
    return {"alien", "ни пути мода, ни обращений к sd:/ — чужой модуль"};
}

std::vector<std::string> occupied_slots(IFileSystem& fs, const std::string& game_exefs) {
    std::vector<std::string> v;
    for (int n = 0; n < 10; n++)
        if (fs.is_file(path_join(game_exefs, "subsdk" + std::to_string(n)))) v.push_back("subsdk" + std::to_string(n));
    return v;
}

InstallPlan plan_install(Env& env, const InstallOptions& opt_in) {
    InstallOptions opt = opt_in;
    InstallPlan plan;
    plan.stamp = env.clock.stamp();
    plan.dest = opt.dest;
    auto& W = plan.warnings;
    auto& L = plan.lines;
    if (opt.swapped_sdk) opt.zbic_fix = true;
    Prep p = prepare(env, opt, W);
    plan.tid = p.tid;
    std::vector<std::string> compat;
    if (!opt.no_autofix) compat = {"hash", "ldn"};
    if (opt.swapped_sdk)
        W.add("это комплект для консоли с ПОДМЕНЁННОЙ библиотекой sdk: несжатый main и правленый main.npdm. Модуль в "
              "нём тот же, что в обычной установке (исправления он включает на консоли сам)", "warn/swapped_kit");
    plan.zbic = zbic_scan(env.fs, p.exefs);
    if (plan.zbic.zbic() && !opt.zbic_fix) {
        ReceiptRead rz = read_receipt(env, p.dest_base);
        std::vector<std::string> already, left;
        for (auto& n : plan.zbic.zbic_files) {
            bool in = false;
            if (rz.sane() && rz.data.contains("zbic_files"))
                for (auto& x : rz.data["zbic_files"]) in |= x == n;
            if (in && env.fs.is_file(path_join(p.dest_exefs, n))) already.push_back(n);
            else left.push_back(n);
        }
        if (!already.empty())
            L.add("сжатие ZBIC у " + join(already) + " уже снято прежним прогоном --zbic-only", "plan/zbic_already",
                  {join(already)});
        if (!left.empty())
            W.add("в exefs игры сжатие ZBIC (" + join(left) + "): Atmosphère младше 1.12.0 такую игру не запустит "
                  "вовсе. Снять: тот же запуск с --zbic-fix", "warn/zbic_left", {join(left)});
    }
    // ---- мод целиком (resolve_mod_input)
    if (!opt.mod.empty() && !opt.mod_files.empty())
        throw Error("FAIL_MOD_ARGS", "мод задаётся одним ключом: --mod или --mod-files, не оба");
    std::string mod_files = opt.mod_files;
    if (!opt.mod.empty()) {
        plan.mod = analyze_mod(env, opt.mod, p.tid, &p.exefs);
    } else if (!mod_files.empty() && env.fs.is_dir(mod_files) && find_mod_tree(env.fs, mod_files) &&
               lower(path_name(mod_files)) != "romfs" && lower(path_name(mod_files)) != "files") {
        L.add("--mod-files указывает на дерево мода — беру мод ЦЕЛИКОМ, как --mod", "plan/mod_tree_whole");
        plan.mod = analyze_mod(env, mod_files, p.tid, &p.exefs);
        mod_files.clear();
    }
    if (plan.mod) {
        std::vector<std::string> clash;
        if (opt.zbic_fix)
            for (auto& [n, src] : plan.mod->exefs)
                if (std::find(plan.zbic.zbic_files.begin(), plan.zbic.zbic_files.end(), n) != plan.zbic.zbic_files.end())
                    clash.push_back(n);
        if (!clash.empty())
            throw Error("FAIL_MOD_ZBIC_CONFLICT", "мод несёт свой " + join(clash) + ", а снятие ZBIC кладёт в exefs файл "
                                                  "с тем же именем. Двум файлам не лечь.", {join(clash)});
        W.append(plan.mod->warnings);
        for (size_t i = 0; i < plan.mod->skipped.size(); ++i)
            W.add("из мода НЕ перенесено: " + plan.mod->skipped[i], "warn/mod_not_moved", {}, {plan.mod->skipped.msg(i)});
    }
    plan.early = plan.mod && !plan.mod->modules.empty();

    // ---- слот
    plan.busy_game = occupied_slots(env.fs, p.exefs);
    auto [slot, updating] = choose_slot(env, opt, plan.busy_game, p.dest_base, p.dest_exefs, W, L,
                                        plan.mod ? &*plan.mod : nullptr);
    plan.slot = slot;
    plan.updating = updating;
    if (plan.mod && !plan.mod->modules.empty()) {
        Msg wm;
        auto [ok, why] = mod_slot_order(slot, *plan.mod, &wm);
        L.add(why, wm.key, wm.args);
        if (!ok) W.add(why, wm.key, wm.args);
    }

    // ---- npdm
    Bytes game_npdm = env.fs.read_file(path_join(p.exefs, "main.npdm"));
    Bytes npdm;
    if (plan.mod && plan.mod->npdm) {
        ModNpdm mn = make_mod_npdm(env.fs.read_file(*plan.mod->npdm), &game_npdm, p.tid_npdm);
        npdm = mn.data;
        plan.npdm_mode = mn.mode;
        if (mn.mode == "as-is") L.add("npdm: файл МОДА как есть — его права уже покрывают наши", "plan/npdm_mod_as_is");
        else L.add("npdm: за основу взят файл МОДА, дописано: " + join(mn.added), "plan/npdm_mod_merged", {join(mn.added)});
        for (auto& d : mn.header_diffs) L.add("npdm мода отличается от npdm игры в шапке: " + d, "plan/npdm_mod_diff", {d});
    } else {
        npdm = make_game_npdm(game_npdm, p.tid_npdm);
        L.add("npdm: бит SdCard + вызовы 0x74, 0x75 в ACI0 и ACID; program id " + tid_hex(p.tid_npdm), "plan/npdm_game",
              {tid_hex(p.tid_npdm)});
    }
    auto npdm_p = std::make_shared<const Bytes>(std::move(npdm));

    // ---- модуль
    const ModuleSource& ms = plan.early ? opt.module_early : opt.module;
    if (ms.path.empty() || !env.fs.is_file(ms.path))
        throw Error("FAIL_NO_MODULE_FILE", "нет файла модуля: " + (ms.path.empty() ? std::string("(не задан)") : ms.path),
                    {ms.path});
    Bytes module_raw = env.fs.read_file(ms.path);
    std::string module_sha = sha256_hex(env.hash, module_raw);
    if (!ms.expect_sha256.empty() && lower(ms.expect_sha256) != module_sha)
        throw Error("FAIL_MODULE_SHA", "sha256 модуля не совпал с описью: " + ms.path, {ms.path});
    if (path_name(ms.path) != slot)
        W.add("файл модуля называется " + path_name(ms.path) + ", а слот выбран " + slot +
                  "; имя файла на SD и есть слот, поэтому он будет скопирован как " + slot +
                  " — убедитесь, что модуль СОБРАН под этот слот (MODULE_NAME влияет на имя и на отчёт диагностики)",
              "warn/module_name_slot", {path_name(ms.path), slot});
    auto cls = classify_nso(module_raw);
    if (cls.first != "ours")
        throw Error("FAIL_MODULE_NOT_OURS", "собственное распознавание не считает этот файл нашим модулем (" + cls.first +
                                                ": " + cls.second + ")");
    plan.module_used = ms.path;

    // ---- файлы мода
    std::vector<std::pair<std::string, std::string>> mod_items;
    if (plan.mod) {
        mod_items = plan.mod->files;
        if (mod_items.empty())
            W.add("в моде нет файлов romfs: подменять с карты нечего, ставятся модуль мода, его права и патчи",
                  "warn/mod_no_romfs");
    } else if (!mod_files.empty()) {
        if (!env.fs.is_dir(mod_files)) throw Error("FAIL_NO_MOD_FILES_DIR", "нет папки файлов мода: " + mod_files, {mod_files});
        rglob(env.fs, mod_files, "", mod_items);
        std::sort(mod_items.begin(), mod_items.end());
        if (mod_items.empty()) throw Error("FAIL_MOD_FILES_EMPTY", "папка файлов мода пуста: " + mod_files, {mod_files});
    } else {
        W.add("мод не задан (ни --mod, ни --mod-files): встанет только модуль, подменять будет нечего", "warn/no_mod");
    }
    if (env.fs.is_dir(path_join(p.dest_base, "romfs")) || env.fs.is_file(path_join(p.dest_base, "romfs.bin")))
        W.add("в " + p.dest_base + " есть romfs/romfs.bin — Atmosphère включит LayeredFS и потратит 12-МБ кучу "
              "ams_mitm; тяжёлые игры на этом зависают. Уберите старый мод на LayeredFS (вручную)",
              "warn/layeredfs", {p.dest_base});

    // ---- ZBIC
    std::map<std::string, std::shared_ptr<const Bytes>> zbic;
    if (opt.zbic_fix) zbic = zbic_prepare(env, p.exefs, plan.zbic, !opt.swapped_sdk, L);

    // ---- подмена sdk (--swap-sdk эталона)
    std::optional<SdkFile> swap;
    if (!opt.swap_sdk.empty()) {
        if (!env.fs.is_file(opt.swap_sdk))
            throw Error("FAIL_SWAP_SDK_BAD_ARG", "--swap-sdk: нет файла " + opt.swap_sdk, {opt.swap_sdk});
        swap = identify_sdk(env, opt.swap_sdk, "swap");
        if (!swap || !swap->nnsdk)
            throw Error("FAIL_SWAP_SDK_NOT_NNSDK", "файл --swap-sdk — не библиотека nnSdk: " + opt.swap_sdk, {opt.swap_sdk});
    }

    // ---- соседи
    ReceiptRead receipt_now = read_receipt(env, p.dest_base);
    std::map<std::string, std::string> mod_exefs;
    if (plan.mod) mod_exefs = plan.mod->exefs;
    std::vector<std::string> to_write{slot, "main.npdm"};
    for (auto& [n, d] : zbic) to_write.push_back(n);
    for (auto& [n, s] : mod_exefs) to_write.push_back(n);
    if (swap) to_write.push_back("sdk");
    std::set<std::string> tw(to_write.begin(), to_write.end());
    auto ours_known = exefs_ours_known(receipt_now);
    auto before = snapshot_sha(env, p.dest_exefs);
    std::map<std::string, std::string> foreign_before;
    for (auto& [n, sha] : before)
        if (!ours_known.count(n) && !has(n, ".bak-") && !has(n, ".removed-")) foreign_before[n] = sha;
    std::set<std::string> same_as_mod;
    for (auto& [n, src] : mod_exefs)
        if (foreign_before.count(n) && foreign_before[n] == file_sha256(env, src)) same_as_mod.insert(n);
    std::set<std::string> repl;
    for (auto& n : to_write)
        if (foreign_before.count(n) && !same_as_mod.count(n)) repl.insert(n);
    plan.replacing_foreign = sorted(repl);
    for (auto& [n, s] : foreign_before)
        if (!tw.count(n)) plan.keeping.push_back(n);
    for (auto& n : plan.replacing_foreign)
        W.add("заменён не наш файл exefs/" + n + ": его клали не мы (копия прежнего рядом, .bak-<время>)",
              "warn/replaced_foreign", {n});
    plan.guard_dir = p.dest_exefs;
    for (auto& [n, sha] : before)
        if (!tw.count(n)) plan.foreign_before[n] = sha;
    // Вид сборки стоящего модуля (квитанция) — для строки плана «отладочный модуль заменится обычным».
    if (receipt_now.sane()) {
        const ojson& r = receipt_now.data;
        if (r.contains("module_variant") && r["module_variant"].is_string())
            plan.prev_variant = r["module_variant"].get<std::string>();
        plan.prev_diag = (r.contains("diag") && r["diag"].is_boolean() && r["diag"].get<bool>()) ||
                         (r.contains("diag_gfx") && r["diag_gfx"].is_boolean() && r["diag_gfx"].get<bool>());
        plan.prev_known = r.contains("slot") && r["slot"].is_string();
    }

    // ---- действия (порядок записи как у эталона с --apply; учёт своего — Ledger эталона)
    const std::string& T = p.tid;
    std::vector<std::string> mod_patch_known;
    if (receipt_now.sane() && receipt_now.data.contains("mod") && receipt_now.data["mod"].is_object())
        for (const char* k : {"patches", "extra"})
            if (receipt_now.data["mod"].contains(k))
                for (auto& x : receipt_now.data["mod"][k]) mod_patch_known.push_back(path_name(x.get<std::string>()));
    std::vector<std::string> exefs_names(ours_known.begin(), ours_known.end());
    exefs_names.insert(exefs_names.end(), mod_patch_known.begin(), mod_patch_known.end());
    bool files_assumed = receipt_now.sane() && !receipt_now.data.contains("owned");
    auto in_exefs = [&](Action a) { a.ours_names = exefs_names; return a; };
    auto outside = [&](Action a) { a.ours_names = mod_patch_known; return a; };
    std::string dest_files = path_join(p.dest_base, "files");
    plan.actions.push_back(mkdir_act(p.dest_exefs));
    plan.actions.push_back(mkdir_act(dest_files));
    if (!updating) {
        Action a;
        a.kind = ActKind::SaveOrig;
        a.src = path_join(p.dest_exefs, "main.npdm");
        a.dst = path_join(p.dest_exefs, "main.npdm.orig");
        a.what = "npdm-orig";
        plan.actions.push_back(a);
    }
    plan.actions.push_back(in_exefs(put_file(ms.path, path_join(p.dest_exefs, slot), "module",
                                             rel_title(T, "exefs/" + slot), module_sha, repl.count(slot) > 0)));
    {
        // Чужой main.npdm, лежавший до нас, сохранён как main.npdm.orig — снятие вернёт его оттуда; поэтому при
        // нашем .orig сам main.npdm — свой и перезаписывается без .bak- (как эталон).
        Action na = in_exefs(put_bytes(npdm_p, path_join(p.dest_exefs, "main.npdm"), "npdm",
                                       rel_title(T, "exefs/main.npdm"), env.hash, repl.count("main.npdm") > 0));
        na.ours_if_owned = path_join(p.dest_exefs, "main.npdm.orig");
        plan.actions.push_back(na);
    }
    for (auto& [n, d] : zbic)
        plan.actions.push_back(in_exefs(put_bytes(d, path_join(p.dest_exefs, n), "zbic-nso", rel_title(T, "exefs/" + n),
                                                  env.hash, repl.count(n) > 0)));
    for (auto& [n, src] : mod_exefs)
        plan.actions.push_back(in_exefs(put_file(src, path_join(p.dest_exefs, n), "mod-exefs", rel_title(T, "exefs/" + n),
                                                 file_sha256(env, src), repl.count(n) > 0)));
    if (plan.mod) {
        for (auto& pt : plan.mod->patches)
            plan.actions.push_back(outside(put_file(pt.path, path_join(path_join(opt.dest, "atmosphere/exefs_patches"), pt.rel),
                                                    "mod-patch", "atmosphere/exefs_patches/" + pt.rel,
                                                    file_sha256(env, pt.path), false)));
        for (auto& [rel, src] : plan.mod->extra)
            plan.actions.push_back(outside(put_file(src, path_join(p.dest_base, rel), "mod-extra", rel_title(T, rel),
                                                    file_sha256(env, src), false)));
    }
    for (auto& [rel, src] : mod_items) {
        Action a = put_file(src, path_join(dest_files, rel), "mod-file", rel_title(T, "files/" + rel), file_sha256(env, src),
                            false);
        a.assume_ours = files_assumed;
        plan.actions.push_back(a);
    }
    std::optional<ojson> swap_rec;
    if (swap) {
        ojson sb = receipt_now.sane() && receipt_now.data.contains("sdk_swap") && receipt_now.data["sdk_swap"].is_object()
                       ? receipt_now.data["sdk_swap"] : ojson::object();
        ojson fb = sb.contains("foreign_backup") ? sb["foreign_backup"] : ojson(nullptr);
        ojson fs_ = sb.contains("foreign_sha256") ? sb["foreign_sha256"] : ojson(nullptr);
        std::string dst = path_join(p.dest_exefs, "sdk");
        Action sa = put_file(swap->path, dst, "sdk", rel_title(T, "exefs/sdk"), swap->sha256, foreign_before.count("sdk") > 0);
        sa.sdk_swap = true;   // копия чужого sdk — в sdk_swap.foreign_backup (ставит исполнитель)
        plan.actions.push_back(sa);
        ojson w = ojson::object();
        w["file"] = "sdk";
        w["module"] = swap->module_name;
        w["version"] = swap->version ? ojson(version_text(*swap->version)) : ojson(nullptr);
        w["sha256"] = swap->sha256;
        w["size"] = swap->size;
        w["source_name"] = path_name(swap->path);
        w["foreign_backup"] = fb;
        w["foreign_sha256"] = fs_;
        w["installed"] = "";
        w["verified_on_console"] = false;
        swap_rec = w;
        const std::string sv = swap->version ? version_text(*swap->version) : std::string("?");
        L.add(dst + "   <- " + swap->path + " (подмена sdk: nnSdk " + w["version"].dump() + ")", "plan/sdk_swap",
              {sv, dst, swap->path});
    }
    // ---- квитанция
    ReceiptFields rf;
    rf.sdk_swap = swap_rec;
    rf.tid = T;
    rf.slot = slot;
    rf.module_sha256 = module_sha;
    rf.npdm_sha256 = sha256_hex(env.hash, *npdm_p);
    rf.module_name = slot;
    for (auto& [n, d] : zbic) rf.zbic_files.push_back(n);
    for (auto& [n, s] : foreign_before)
        if (!mod_exefs.count(n) && !(swap && n == "sdk")) rf.neighbours.push_back(n);
    rf.mode = "install";
    rf.diag = opt.diag;
    rf.diag_gfx = opt.diag_gfx;
    rf.module_variant = module_variant(!compat.empty(), opt.diag, opt.diag_gfx, plan.early);
    plan.module_variant = *rf.module_variant;
    rf.compat = compat;
    rf.version = opt.pipeline_version;
    rf.installed_by = opt.installed_by;
    if (plan.mod) {
        ojson mi = ojson::object();
        mi["source"] = plan.mod->root;
        mi["exefs"] = sorted([&] { std::vector<std::string> v; for (auto& [n, s] : mod_exefs) v.push_back(n); return v; }());
        std::vector<std::string> pr, ex;
        for (auto& pt : plan.mod->patches) pr.push_back(pt.rel);
        for (auto& [r, s] : plan.mod->extra) ex.push_back(r);
        mi["patches"] = sorted(pr);
        mi["extra"] = sorted(ex);
        mi["npdm"] = plan.npdm_mode;
        mi["early_fs"] = plan.early;
        mi["files"] = mod_items.size();
        rf.mod = mi;
    }
    Action ra;
    ra.kind = ActKind::PutReceipt;
    ra.dst = path_join(p.dest_base, kReceiptName);
    ra.what = "receipt";
    ra.receipt = make_receipt(rf, receipt_now);
    ra.receipt_base = receipt_now.sane() ? receipt_now.data : ojson::object();
    ra.installed_by = opt.installed_by;
    plan.actions.push_back(ra);

    // ---- человеческий план
    L.add(path_join(p.dest_exefs, slot) + "   <- " + ms.path + " (модуль)", "plan/put_module",
          {path_join(p.dest_exefs, slot), ms.path});
    // Смена вида сборки: модуль перезаписывается начисто (без .bak), план говорит это словами.
    const bool new_diag = opt.diag || opt.diag_gfx;
    if (plan.prev_known && plan.prev_diag && !new_diag)
        L.add("отладочный модуль заменится обычным (перезапись начисто, без копии)", "plan/module_to_plain");
    else if (plan.prev_known && !plan.prev_diag && new_diag)
        L.add("обычный модуль заменится отладочным (перезапись начисто, без копии)", "plan/module_to_diag");
    L.add(path_join(p.dest_exefs, "main.npdm") + "   <- правленый main.npdm игры", "plan/put_npdm",
          {path_join(p.dest_exefs, "main.npdm")});
    for (auto& [n, d] : zbic)
        L.add(path_join(p.dest_exefs, n) + "   <- " + n + " игры без сжатия ZBIC", "plan/put_zbic", {path_join(p.dest_exefs, n), n});
    for (auto& [n, s] : mod_exefs)
        L.add(path_join(p.dest_exefs, n) + "   <- exefs мода", "plan/put_mod_exefs", {path_join(p.dest_exefs, n)});
    if (plan.mod)
        for (auto& pt : plan.mod->patches)
            L.add("atmosphere/exefs_patches/" + pt.rel + "   <- патч мода" + (pt.target ? " -> " + *pt.target : ""),
                  "plan/put_mod_patch", {"atmosphere/exefs_patches/" + pt.rel});
    if (!mod_items.empty())
        L.add(dest_files + "/...   <- " + std::to_string(mod_items.size()) + " файлов мода", "plan/put_mod_files",
              {dest_files + "/", std::to_string(mod_items.size())});
    L.add(ra.dst + "   <- квитанция (слот, версия, sha256)", "plan/put_receipt", {ra.dst});
    if (!plan.keeping.empty())
        L.add("остаются нетронутыми (не наши файлы): " + join(plan.keeping), "plan/keeping", {join(plan.keeping)});
    return plan;
}

InstallPlan plan_zbic_only(Env& env, const InstallOptions& opt) {
    InstallPlan plan;
    plan.stamp = env.clock.stamp();
    plan.dest = opt.dest;
    Prep p = prepare(env, opt, plan.warnings);
    plan.tid = p.tid;
    plan.zbic = zbic_scan(env.fs, p.exefs);
    if (!plan.zbic.ok || !plan.zbic.zbic())
        throw Error("FAIL_ZBIC_NOT_FOUND", "сжатия ZBIC в " + p.exefs + " нет — снимать нечего", {p.exefs});
    ReceiptRead rec = read_receipt(env, p.dest_base);
    if (rec.present && rec.broken)
        plan.warnings.add(std::string("квитанция ") + kReceiptName + " не читается (" + rec.broken_why +
                              "); прежние записи в неё не переносятся", "warn/zbic_receipt_broken", {kReceiptName});
    for (auto& [s, cw] : scan_dest(env, p.dest_exefs))
        plan.lines.add(s + ": " + label_of(cw.first) + " — " + cw.second, label_key(cw.first), {s});
    auto zbic = zbic_prepare(env, p.exefs, plan.zbic, true, plan.lines);
    std::vector<std::string> to_write;
    for (auto& [n, d] : zbic) to_write.push_back(n);
    std::set<std::string> tw(to_write.begin(), to_write.end());
    auto ours_known = exefs_ours_known(rec);
    auto before = snapshot_sha(env, p.dest_exefs);
    std::map<std::string, std::string> foreign_before;
    for (auto& [n, sha] : before)
        if (!ours_known.count(n) && !has(n, ".bak-") && !has(n, ".removed-")) foreign_before[n] = sha;
    std::set<std::string> repl;
    for (auto& n : to_write)
        if (foreign_before.count(n)) repl.insert(n);
    plan.replacing_foreign = sorted(repl);
    for (auto& [n, s] : foreign_before)
        if (!tw.count(n)) plan.keeping.push_back(n);
    for (auto& n : plan.replacing_foreign)
        plan.warnings.add("заменён не наш файл exefs/" + n + ": его клали не мы (копия прежнего рядом, .bak-<время>)",
                          "warn/replaced_foreign", {n});
    plan.guard_dir = p.dest_exefs;
    for (auto& [n, sha] : before)
        if (!tw.count(n)) plan.foreign_before[n] = sha;
    plan.actions.push_back(mkdir_act(p.dest_exefs));
    for (auto& [n, d] : zbic) {
        Action a = put_bytes(d, path_join(p.dest_exefs, n), "zbic-nso", rel_title(p.tid, "exefs/" + n), env.hash,
                             repl.count(n) > 0);
        a.ours_names.assign(ours_known.begin(), ours_known.end());
        plan.actions.push_back(a);
    }
    ReceiptFields rf;
    rf.tid = p.tid;
    rf.zbic_files = to_write;
    for (auto& [n, s] : foreign_before) rf.neighbours.push_back(n);
    rf.mode = "zbic-only";
    rf.version = opt.pipeline_version;
    rf.installed_by = opt.installed_by;
    Action ra;
    ra.kind = ActKind::PutReceipt;
    ra.dst = path_join(p.dest_base, kReceiptName);
    ra.what = "receipt";
    ra.receipt = make_receipt(rf, rec);
    ra.receipt_base = rec.sane() ? rec.data : ojson::object();
    ra.installed_by = opt.installed_by;
    plan.actions.push_back(ra);
    for (auto& n : to_write)
        plan.lines.add(path_join(p.dest_exefs, n) + "   <- " + n + " игры без сжатия ZBIC", "plan/put_zbic",
                       {path_join(p.dest_exefs, n), n});
    plan.lines.add(ra.dst + "   <- квитанция (снятые файлы)", "plan/put_receipt_zbic", {ra.dst});
    plan.lines.add("больше НИЧЕГО не кладётся: ни subsdkN мода, ни main.npdm, ни files/", "plan/zbic_only_nothing_else");
    return plan;
}

InstalledState inspect_installed(Env& env, const std::string& dest, const std::string& tid) {
    InstalledState s;
    s.tid = upper(tid);
    s.title_dir = title_dir_of(dest, tid);
    s.dir_exists = env.fs.is_dir(s.title_dir);
    if (!s.dir_exists) return s;
    s.receipt = read_receipt(env, s.title_dir);
    std::string ex = path_join(s.title_dir, "exefs");
    s.slots = scan_dest(env, ex);
    if (env.fs.is_dir(ex)) {
        s.has_npdm_orig = env.fs.is_file(path_join(ex, "main.npdm.orig"));
        for (auto& e : env.fs.list_dir(ex))
            if (has(e.name, ".bak-") || has(e.name, ".removed-")) s.deferred.push_back(e.name);
    }
    s.has_romfs = env.fs.is_dir(path_join(s.title_dir, "romfs")) || env.fs.is_file(path_join(s.title_dir, "romfs.bin"));
    if (s.receipt.sane() && s.receipt.data.contains("module_sha256") && s.receipt.data["module_sha256"].is_string() &&
        s.receipt.data.contains("slot") && s.receipt.data["slot"].is_string()) {
        std::string f = path_join(ex, s.receipt.data["slot"].get<std::string>());
        s.module_matches = env.fs.is_file(f) && file_sha256(env, f) == s.receipt.data["module_sha256"].get<std::string>();
    }
    return s;
}

std::optional<bool> installed_matches_game(Env& env, const std::string& dest, const std::string& tid,
                                           const std::string& game_exefs, std::vector<std::string>* stale) {
    std::string base = title_dir_of(dest, tid), ex = path_join(base, "exefs");
    ReceiptRead rec = read_receipt(env, base);
    if (!rec.sane() || !rec.data.contains("zbic_files") || !rec.data["zbic_files"].is_array()) return std::nullopt;
    std::optional<bool> res;
    for (auto& z : rec.data["zbic_files"]) {
        std::string n = z.get<std::string>(), mine = path_join(ex, n), game = path_join(game_exefs, n);
        if (!env.fs.is_file(mine) || !env.fs.is_file(game)) continue;
        Bytes a = env.fs.read_range(mine, 0, kNsoHeaderSize), b = env.fs.read_range(game, 0, kNsoHeaderSize);
        NsoHeader ha, hb;
        if (!parse_nso_header(a.data(), a.size(), ha) || !parse_nso_header(b.data(), b.size(), hb)) continue;
        bool same = ha.module_id == hb.module_id;
        if (!same && stale) stale->push_back(n);
        res = res.value_or(true) && same;
    }
    return res;
}

Plan plan_uninstall(Env& env, const std::string& dest, const std::string& tid_in, bool also_files) {
    (void)also_files;   // как у эталона: файлы мода удаляются по квитанции всегда
    Plan plan;
    plan.stamp = env.clock.stamp();
    plan.dest = dest;
    std::string tid = upper(tid_in);
    std::string base = title_dir_of(dest, tid), ex = path_join(base, "exefs");
    auto& L = plan.lines;
    auto& W = plan.warnings;
    if (!env.fs.is_dir(base)) throw Error("FAIL_UNINSTALL_NO_DIR", "папки нет: " + base + " — снимать нечего", {base});
    ReceiptRead rec = read_receipt(env, base);
    auto found = scan_dest(env, ex);
    if (!rec.sane())
        throw Error("FAIL_UNINSTALL_NOTHING",
                    std::string(rec.present ? "квитанция не читается (" + rec.broken_why + ")" : "квитанции нет") +
                        " — снимать нечего: без квитанции ничего не удаляется (нечем доказать, что файл наш)");
    const ojson& d = rec.data;
    auto str = [](const ojson& o, const char* k) {
        return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
    };
    std::string slot = str(d, "slot");
    if (!slot.empty() && found.count(slot) && found[slot].first != "ours")
        throw Error("FAIL_UNINSTALL_FOREIGN_IN_SLOT", "в слоте " + slot + " из квитанции лежит НЕ наш модуль (" +
                                                          found[slot].second + "). Ничего не трогаю.", {slot});
    // receipt_items эталона: {путь от назначения: sha256 | ""}
    std::string rb = "atmosphere/contents/" + tid, rx = rb + "/exefs";
    std::map<std::string, std::string> items;
    auto setdef = [&](const std::string& k, const std::string& v) {
        if (!items.count(k)) items[k] = v;
    };
    if (d.contains("owned") && d["owned"].is_object())
        for (auto& [k, v] : d["owned"].items()) setdef(k, v.is_string() ? v.get<std::string>() : "");
    if (!slot.empty()) setdef(rx + "/" + slot, str(d, "module_sha256"));
    if (!str(d, "npdm_sha256").empty()) setdef(rx + "/main.npdm", str(d, "npdm_sha256"));
    if (d.contains("zbic_files") && d["zbic_files"].is_array())
        for (auto& z : d["zbic_files"]) setdef(rx + "/" + z.get<std::string>(), "");
    if (d.contains("mod") && d["mod"].is_object()) {
        const ojson& m = d["mod"];
        if (m.contains("exefs")) for (auto& x : m["exefs"]) setdef(rx + "/" + x.get<std::string>(), "");
        if (m.contains("patches")) for (auto& x : m["patches"]) setdef("atmosphere/exefs_patches/" + x.get<std::string>(), "");
        if (m.contains("extra")) for (auto& x : m["extra"]) setdef(rb + "/" + x.get<std::string>(), "");
    }
    const ojson swap = d.contains("sdk_swap") && d["sdk_swap"].is_object() ? d["sdk_swap"] : ojson::object();
    if (!str(swap, "file").empty()) setdef(rx + "/" + str(swap, "file"), str(swap, "sha256"));
    if (d.contains("own_backups") && d["own_backups"].is_object())
        for (auto& [k, v] : d["own_backups"].items()) setdef(k, v.is_string() ? v.get<std::string>() : "");

    std::string npdm_rel = rx + "/main.npdm", orig_rel = rx + "/main.npdm.orig";
    std::string orig_sha;
    if (items.count(orig_rel)) { orig_sha = items[orig_rel]; items.erase(orig_rel); }
    std::string orig_p = path_join(dest, orig_rel);
    bool orig_ok = env.fs.is_file(orig_p) && (orig_sha.empty() || file_sha256(env, orig_p) == orig_sha);
    auto safe = [](const std::string& r) {
        return !r.empty() && r[0] != '/' && r.find("..") == std::string::npos && r.find(':') == std::string::npos;
    };
    std::vector<std::string> to_delete, changed, no_sha, gone, bad;
    for (auto& [rel, sha] : items) {   // sorted(items.items())
        std::string p = path_join(dest, rel);
        if (!safe(rel)) bad.push_back(rel);
        else if (!env.fs.is_file(p)) gone.push_back(rel);
        else if (sha.empty()) no_sha.push_back(rel);
        else if (file_sha256(env, p) != sha) changed.push_back(rel);
        else to_delete.push_back(rel);
    }
    std::set<std::string> deleting(to_delete.begin(), to_delete.end());
    bool npdm_back = deleting.count(npdm_rel) && orig_ok;
    std::map<std::string, std::string> foreign;
    if (d.contains("foreign_backups") && d["foreign_backups"].is_object())
        for (auto& [k, v] : d["foreign_backups"].items()) foreign[k] = v.get<std::string>();
    if (!str(swap, "file").empty() && !str(swap, "foreign_backup").empty() && !foreign.count(rx + "/" + str(swap, "file")))
        foreign[rx + "/" + str(swap, "file")] = rx + "/" + str(swap, "foreign_backup");
    std::vector<std::pair<std::string, std::string>> restore, blocked;
    for (auto& [target, bak] : foreign) {
        if (!safe(target) || !safe(bak) || !env.fs.is_file(path_join(dest, bak))) continue;
        if ((target == npdm_rel && npdm_back) || (env.fs.exists(path_join(dest, target)) && !deleting.count(target)))
            blocked.emplace_back(target, bak);
        else
            restore.emplace_back(target, bak);
    }
    std::vector<std::string> created;
    if (d.contains("created_dirs") && d["created_dirs"].is_array()) {
        for (auto& x : d["created_dirs"]) created.push_back(x.get<std::string>());
    } else {   // старая квитанция без created_dirs: только стандартные папки раскладки и только пустые
        std::string fd = path_join(base, "files");
        std::function<void(const std::string&, const std::string&)> walkd = [&](const std::string& dir,
                                                                                const std::string& r) {
            for (auto& e : env.fs.list_dir(dir))
                if (e.is_dir) {
                    created.push_back(r + "/" + e.name);
                    walkd(path_join(dir, e.name), r + "/" + e.name);
                }
        };
        if (env.fs.is_dir(fd)) walkd(fd, rb + "/files");
        created.push_back(rb + "/files");
        created.push_back(rx);
        created.push_back(rb);
    }
    std::sort(created.begin(), created.end());
    created.erase(std::unique(created.begin(), created.end()), created.end());
    std::stable_sort(created.begin(), created.end(), [](const std::string& a, const std::string& b) {
        auto depth = [](const std::string& s) { return std::count(s.begin(), s.end(), '/'); };
        return depth(a) > depth(b);
    });

    for (auto& rel : to_delete) {
        Action a;
        a.kind = ActKind::Delete;
        a.dst = path_join(dest, rel);
        a.rel = rel;
        a.expect_sha256 = items[rel];
        a.what = "uninstall";
        plan.actions.push_back(a);
        L.add("будет удалено: " + rel, "plan/un_delete", {rel});
    }
    if (npdm_back) {
        Action a;
        a.kind = ActKind::Rename;
        a.src = orig_p;
        a.dst = path_join(dest, npdm_rel);
        a.what = "restore";
        plan.actions.push_back(a);
        L.add("main.npdm вернётся из main.npdm.orig (исходный npdm, сохранённый при первой установке)", "plan/un_npdm_restore");
    } else if (deleting.count(npdm_rel)) {
        L.add("исходного main.npdm в назначении нет — игра возьмёт свой npdm из NCA (расширенные права "
              "больше не действуют)", "plan/un_npdm_none");
    }
    for (auto& [target, bak] : restore) {
        Action a;
        a.kind = ActKind::Rename;
        a.src = path_join(dest, bak);
        a.dst = path_join(dest, target);
        a.what = "restore";
        plan.actions.push_back(a);
        L.add("вернётся на место чужой файл: " + target + " <- " + path_name(bak), "plan/un_restore_foreign",
              {target, path_name(bak)});
    }
    for (auto& [target, bak] : blocked)
        W.add("копия чужого файла " + bak + " остаётся (место " + target + " занято)", "warn/un_backup_blocked", {bak, target});
    for (auto& r : changed)
        W.add("НЕ удаляется: " + r + " — файл изменён после установки (sha256 не совпал)", "warn/un_changed", {r});
    for (auto& r : no_sha) W.add("НЕ удаляется: " + r + " — квитанция не помнит его sha256, сверить нечем", "warn/un_no_sha", {r});
    for (auto& r : bad) W.add("НЕ удаляется: " + r + " — путь выходит за назначение", "warn/un_bad_path", {r});
    for (auto& r : gone) L.add("уже нет: " + r, "plan/un_gone", {r});
    std::string rp = path_join(base, kReceiptName);
    if (env.fs.is_file(rp)) {
        Action a;
        a.kind = ActKind::Delete;
        a.dst = rp;
        a.rel = rb + "/" + kReceiptName;
        a.expect_sha256 = file_sha256(env, rp);
        a.what = "receipt";
        plan.actions.push_back(a);
        L.add("будет удалено: " + a.rel + " (квитанция)", "plan/un_receipt", {a.rel});
    }
    for (auto& r : created) {
        if (!safe(r)) continue;
        Action a;
        a.kind = ActKind::RemoveEmptyDir;
        a.dst = path_join(dest, r);
        a.rel = r;
        a.what = "dir";
        plan.actions.push_back(a);
    }
    L.add("пустые папки, созданные установкой, будут убраны", "plan/un_dirs");
    return plan;
}

}  // namespace mk
