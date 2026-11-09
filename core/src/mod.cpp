// Мод целиком: порт find_mod_tree, mod_module_info, analyze_mod, mod_slot_order (эталон; сверка — tests/host/compare_pc.py).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/mod.hpp"
#include "mk/nso.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace mk {

namespace {

bool is_hex(const std::string& s, size_t lo, size_t hi) {
    if (s.size() < lo || s.size() > hi) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
bool hex16(const std::string& s) { return is_hex(s, 16, 16); }
std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* kOpenFile = "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci";

std::vector<std::string> module_names() {
    std::vector<std::string> v{"main", "rtld", "sdk"};
    for (int i = 0; i < 10; i++) v.push_back("subsdk" + std::to_string(i));
    return v;
}

// rglob("*"): все файлы под d -> (путь от d через '/', полный путь), по пути побайтно.
void walk(IFileSystem& fs, const std::string& d, const std::string& rel,
          std::vector<std::pair<std::string, std::string>>& out) {
    for (auto& e : fs.list_dir(d)) {
        std::string p = path_join(d, e.name), r = rel.empty() ? e.name : rel + "/" + e.name;
        if (e.is_dir) walk(fs, p, r, out);
        else out.emplace_back(r, p);
    }
}
std::vector<std::pair<std::string, std::string>> rglob(IFileSystem& fs, const std::string& d) {
    std::vector<std::pair<std::string, std::string>> out;
    walk(fs, d, "", out);
    std::sort(out.begin(), out.end());
    return out;
}

std::string build_id(IFileSystem& fs, const std::string& p) {
    if (!fs.is_file(p)) return "";
    Bytes h = fs.read_range(p, 0, 0x100);
    if (h.size() < 0x60 || std::memcmp(h.data(), "NSO0", 4) != 0) return "";
    return hex(h.data() + 0x40, 32, true);
}

ModModuleInfo module_info(Env& env, const std::string& path, const std::string* game_exefs) {
    ModModuleInfo i;
    i.name = path_name(path);
    Bytes raw = env.fs.read_file(path);
    i.size = raw.size();
    i.sha256 = sha256_hex(env.hash, raw);
    if (game_exefs && env.fs.is_file(path_join(*game_exefs, i.name))) i.replaces_game = true;
    NsoFlat n;
    try {
        n = parse_nso_flat(raw);
    } catch (const std::exception& e) {
        i.problem = std::string("не разбирается как NSO: ") + e.what();
        return i;
    }
    i.nso = true;
    i.build_id = hex(n.header.module_id.data(), 32, true);
    uint64_t strtab = n.tags.count(5) ? n.tags.at(5) : 0, strsz = n.tags.count(10) ? n.tags.at(10) : 0;
    const Bytes& im = n.image;
    size_t L = std::strlen(kOpenFile);
    for (size_t p = 0; p + L <= im.size();) {
        const void* f = std::memchr(im.data() + p, kOpenFile[0], im.size() - p);
        if (!f) break;
        size_t at = static_cast<const uint8_t*>(f) - im.data();
        if (at + L <= im.size() && std::memcmp(im.data() + at, kOpenFile, L) == 0) {
            if (strtab <= at && at < strtab + strsz) i.imports_openfile = true;
            else i.hooks_openfile = true;
            p = at + L;
        } else {
            p = at + 1;
        }
    }
    return i;
}

}  // namespace

int slot_number(const std::string& name) {
    if (name.size() == 7 && name.compare(0, 6, "subsdk") == 0 && name[6] >= '0' && name[6] <= '9') return name[6] - '0';
    return -1;
}

std::optional<ModTree> find_mod_tree(IFileSystem& fs, const std::string& p) {
    if (!fs.is_dir(p)) return std::nullopt;
    std::optional<std::string> atm;
    std::string a1 = path_join(p, "atmosphere"), c1 = path_join(p, "contents");
    if (fs.is_dir(a1)) atm = a1;
    else if (lower(path_name(p)) == "atmosphere") atm = p;
    else if (fs.is_dir(c1)) {
        for (auto& e : fs.list_dir(c1))
            if (e.is_dir && hex16(e.name)) { atm = p; break; }
    }
    if (atm) {
        ModTree t;
        t.root = p;
        t.atmosphere = atm;
        std::string contents = path_join(*atm, "contents");
        if (fs.is_dir(contents))
            for (auto& e : fs.list_dir(contents))
                if (e.is_dir && hex16(e.name)) t.titles[upper(e.name)] = path_join(contents, e.name);
        return t;
    }
    std::string nm = path_name(p);
    if (hex16(nm) && (fs.exists(path_join(p, "exefs")) || fs.exists(path_join(p, "romfs")) ||
                      fs.exists(path_join(p, "romfs.bin")))) {
        ModTree t;
        t.root = p;
        t.titles[upper(nm)] = p;
        return t;
    }
    return std::nullopt;
}

ModPlan analyze_mod(Env& env, const std::string& mod_path, const std::string& tid_in, const std::string* game_exefs) {
    IFileSystem& fs = env.fs;
    std::string tid = upper(tid_in);
    auto tree = find_mod_tree(fs, mod_path);
    if (!tree)
        throw Error("FAIL_MOD_NOT_TREE", "папка мода не похожа на мод: в ней нет atmosphere/contents/<TID>/ "
                                         "(или contents/<TID>/, или папки <TID> с exefs/romfs): " + mod_path,
                    {mod_path});
    if (!tree->titles.count(tid)) {
        std::string have;
        for (auto& [k, v] : tree->titles) have += (have.empty() ? "" : ", ") + k;
        throw Error("FAIL_MOD_OTHER_TITLE", "в моде нет папки этой игры (" + tid + "): в нём " +
                                                (have.empty() ? "ничего" : have), {tid, have.empty() ? "—" : have});
    }
    std::string tdir = tree->titles[tid];
    ModPlan plan;
    plan.root = tree->root;
    plan.title_dir = tdir;
    if (fs.is_file(path_join(tdir, "romfs.bin")))
        throw Error("FAIL_MOD_ROMFS_BIN", "в моде лежит romfs.bin (упакованный образ romfs): разложить его по "
                                          "файлам нельзя, а положить как есть — значит включить LayeredFS");
    auto names = module_names();
    std::string exefs_dir = path_join(tdir, "exefs");
    if (fs.is_dir(exefs_dir)) {
        for (auto& e : fs.list_dir(exefs_dir)) {
            std::string p = path_join(exefs_dir, e.name);
            if (e.is_dir) {
                plan.skipped.add("exefs/" + e.name + "/ — папка внутри exefs (Loader её не читает), не переносится",
                                 "skip/exefs_dir", {e.name});
                continue;
            }
            if (e.name == "main.npdm") { plan.npdm = p; continue; }
            plan.exefs[e.name] = p;
            if (game_exefs && fs.is_file(path_join(*game_exefs, e.name))) plan.replaces.push_back(e.name);
            if (slot_number(e.name) >= 0) plan.modules[e.name] = module_info(env, p, game_exefs);
            else if (std::find(names.begin(), names.end(), e.name) == names.end())
                plan.warnings.add("в exefs мода лежит " + e.name +
                                      " — это не имя модуля (Loader его не грузит); переносится как есть",
                                  "warn/mod_exefs_unknown", {e.name});
        }
    }
    std::map<std::string, std::string> seen;
    for (const char* sub : {"romfs", "files"}) {
        std::string d = path_join(tdir, sub);
        if (!fs.is_dir(d)) continue;
        for (auto& [rel, p] : rglob(fs, d)) {
            auto it = seen.find(rel);
            if (it != seen.end() && file_sha256(env, it->second) != file_sha256(env, p))
                throw Error("FAIL_MOD_FILES_CLASH", "в моде один и тот же файл лежит и в romfs/, и в files/, и они "
                                                    "РАЗНЫЕ: " + rel, {rel});
            seen[rel] = p;
        }
    }
    plan.files.assign(seen.begin(), seen.end());

    std::set<std::string> extra_tops;
    for (auto& e : fs.list_dir(tdir)) {
        if (e.name == "exefs" || e.name == "romfs" || e.name == "files" || e.name == "romfs.bin" ||
            e.name == "nxredirect.json")
            continue;
        if (e.name.find(".bak-") != std::string::npos || e.name.find(".removed-") != std::string::npos) continue;
        std::string p = path_join(tdir, e.name);
        if (e.is_dir) {
            for (auto& [rel, q] : rglob(fs, p)) plan.extra.emplace_back(e.name + "/" + rel, q);
        } else {
            plan.extra.emplace_back(e.name, p);
        }
    }
    if (!plan.extra.empty()) {
        for (auto& [r, p] : plan.extra) extra_tops.insert(r.substr(0, r.find('/')));
        std::string s;
        for (auto& t : extra_tops) s += (s.empty() ? "" : ", ") + t;
        plan.warnings.add("в папке игры мода есть и другое (" + s + ") — переносится как есть, рядом с exefs/ и files/",
                          "warn/mod_extra", {s});
    }

    std::map<std::string, std::string> build_ids;
    if (game_exefs)
        for (auto& n : names) {
            std::string b = build_id(fs, path_join(*game_exefs, n));
            if (!b.empty()) build_ids[b] = n + " игры";
        }
    for (auto& [n, p] : plan.exefs) {
        std::string b = build_id(fs, p);
        if (!b.empty()) build_ids[b] = n + " мода";
    }
    if (tree->atmosphere) {
        const std::string& atm = *tree->atmosphere;
        for (auto& e : fs.list_dir(atm)) {
            std::string p = path_join(atm, e.name);
            if (e.name == "contents") {
                if (e.is_dir)
                    for (auto& c : fs.list_dir(p))
                        if (upper(c.name) != tid)
                            plan.skipped.add("atmosphere/contents/" + c.name +
                                                 " — другая программа (не эта игра), не переносится",
                                             "skip/other_title", {"atmosphere/contents/" + c.name});
                continue;
            }
            if (e.name == "exefs_patches" && e.is_dir) {
                for (auto& [rel, q] : rglob(fs, p)) {
                    ModPatch mp{rel, q, std::nullopt};
                    std::string nm = path_name(q);
                    size_t dot = nm.rfind('.');
                    std::string stem = dot == std::string::npos || dot == 0 ? nm : nm.substr(0, dot);
                    std::string suf = dot == std::string::npos || dot == 0 ? "" : lower(nm.substr(dot));
                    if (suf == ".ips" && is_hex(stem, 1, 64)) {
                        std::string want = upper(stem);
                        want.resize(64, '0');
                        auto it = build_ids.find(want);
                        if (it != build_ids.end()) mp.target = it->second;
                        else if (!build_ids.empty())
                            plan.warnings.add("IPS " + rel + " не подходит ни к одному модулю этой версии игры "
                                                  "(build id " + stem.substr(0, 16) + "…): Atmosphère его просто не "
                                                  "применит — возможно, мод для другой версии",
                                              "warn/ips_mismatch", {rel});
                    }
                    plan.patches.push_back(mp);
                }
                continue;
            }
            plan.skipped.add("atmosphere/" + e.name + (e.is_dir ? "/" : "") +
                                 " — не относится к папке игры (системные модули, kip, настройки Atmosphère), "
                                 "не переносится; если он нужен моду, поставьте его сами",
                             "skip/atmosphere_other", {"atmosphere/" + e.name + (e.is_dir ? "/" : "")});
        }
        if (tree->root != atm)
            for (auto& e : fs.list_dir(tree->root)) {
                if (path_join(tree->root, e.name) == atm) continue;
                plan.skipped.add(e.name + (e.is_dir ? "/" : "") +
                                     " — вне atmosphere/, не переносится (homebrew, оверлеи, загрузчик — ставятся отдельно)",
                                 "skip/outside", {e.name + (e.is_dir ? "/" : "")});
            }
    }
    for (auto& n : plan.replaces)
        plan.warnings.add("мод ЗАМЕНЯЕТ файл игры exefs/" + n + " своим — так задумал автор мода; переносится как есть",
                          "warn/mod_replaces", {n});
    for (auto& [n, info] : plan.modules) {
        if (!info.nso)
            plan.warnings.add("модуль мода " + n + " не разбирается как NSO (" + info.problem + "): Loader на нём споткнётся",
                              "warn/mod_module_not_nso", {n});
        if (info.hooks_openfile)
            plan.warnings.add("модуль мода " + n + ", похоже, сам перехватывает nn::fs::OpenFile (ищет его по "
                                  "имени): наш перехват и его встанут цепочкой; если файлы мода не подменяются, "
                                  "дело может быть в этом",
                              "warn/mod_module_hooks", {n});
    }
    if (plan.files.empty() && plan.exefs.empty() && plan.patches.empty())
        throw Error("FAIL_MOD_EMPTY", "в моде для " + tid + " нет ничего, что можно перенести: ни файлов romfs, ни exefs, ни патчей",
                    {tid});
    return plan;
}

std::pair<bool, std::string> mod_slot_order(const std::string& slot, const ModPlan& plan, Msg* msg) {
    int mine = slot_number(slot);
    if (plan.modules.empty() || mine < 0) return {true, ""};
    int mx = -1;
    std::string list;
    for (auto& [n, i] : plan.modules) {
        mx = std::max(mx, slot_number(n));
        list += (list.empty() ? "" : ", ") + n;
    }
    if (msg) *msg = Msg{mine > mx ? "plan/slot_order_ok" : "plan/slot_order_bad", {slot, list}, {}};
    if (mine > mx) return {true, "наш " + slot + " старше модулей мода (" + list + "): инициализируется раньше них"};
    return {false, "наш " + slot + " МЛАДШЕ модуля мода (" + list + "): тот инициализируется раньше нас. Файлы, которые "
                   "он читает в СВОЕЙ инициализации, подмена не увидит; в его хуке nnMain — увидит (ранний перехват)"};
}

}  // namespace mk
