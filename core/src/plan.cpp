// Исполнитель плана: порт Ledger / write_receipt эталона (учёт своего: owned, own_backups,
// foreign_backups, created_dirs), .bak-копии чужого, гейты после записи. Как в
// эталоне, свои файлы перезаписываются начисто, без .bak; own_backups не пополняется.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/plan.hpp"
#include "mk/receipt.hpp"

#include <algorithm>
#include <set>

namespace mk {

std::map<std::string, std::string> snapshot_sha(Env& env, const std::string& dir) {
    std::map<std::string, std::string> m;
    if (!env.fs.is_dir(dir)) return m;
    for (auto& e : env.fs.list_dir(dir))
        if (!e.is_dir) m[e.name] = file_sha256(env, path_join(dir, e.name));
    return m;
}

namespace {

ojson obj_or_empty(const ojson& r, const char* k) {
    return r.is_object() && r.contains(k) && r[k].is_object() ? r[k] : ojson::object();
}

struct Exec {
    Exec(Env& e, const Plan& p) : env(e), plan(p) {}
    Env& env;
    const Plan& plan;
    ApplyReport rep;
    // Ledger
    ojson prior_owned = ojson::object();
    ojson owned = ojson::object(), foreign_backups = ojson::object();
    std::set<std::string> created;
    ojson swap_foreign;   // null или {"backup": имя, "sha256": …} — чужой exefs/sdk, отложенный подменой

    std::string rel_of(const std::string& p) const {
        std::string d = plan.dest;
        if (!d.empty() && d.back() != '/') d += '/';
        if (!d.empty() && p.compare(0, d.size(), d) == 0) return p.substr(d.size());
        return p;
    }
    bool inside_dest(const std::string& p) const {
        std::string d = plan.dest;
        if (!d.empty() && d.back() != '/') d += '/';
        return !d.empty() && p.size() > d.size() && p.compare(0, d.size(), d) == 0;
    }

    void mkdirs(const std::string& dir) {
        if (dir.empty() || env.fs.is_dir(dir)) return;
        std::vector<std::string> fresh;
        for (std::string d = dir; !d.empty() && !env.fs.exists(d) && inside_dest(d); d = path_parent(d)) {
            fresh.push_back(d);
            if (path_parent(d) == d) break;
        }
        env.fs.make_dirs(dir);
        for (auto& f : fresh) created.insert(rel_of(f));
    }

    void own(const std::string& p) { owned[rel_of(p)] = file_sha256(env, p); }

    bool owned_now(const std::string& p) const {
        std::string rel = rel_of(p);
        return prior_owned.contains(rel) || owned.contains(rel);
    }

    bool is_ours(const Action& a) {
        return a.assume_ours || (!a.ours_if_owned.empty() && owned_now(a.ours_if_owned)) || owned_now(a.dst) ||
               std::find(a.ours_names.begin(), a.ours_names.end(), path_name(a.dst)) != a.ours_names.end();
    }

    // Копия .bak- ЧУЖОГО файла (снятие вернёт его). В прежних квитанциях сюда шли и копии своих (own_backups).
    void backed_up(const std::string& original, const std::string& bak) {
        if (!foreign_backups.contains(rel_of(original))) foreign_backups[rel_of(original)] = rel_of(bak);
    }

    std::string sha_of(const Action& a) {
        if (!a.expect_sha256.empty()) return a.expect_sha256;
        if (a.data) return sha256_hex(env.hash, *a.data);
        return file_sha256(env, a.src);
    }

    // Прежний файл на месте (свой — начисто; чужой — уже скопирован в .bak-) — перезапись, иначе новая запись.
    void write(const Action& a) {
        const bool over = env.fs.is_file(a.dst);
        if (a.data) {
            if (over) env.fs.write_file_replace(a.dst, a.data->data(), a.data->size());
            else env.fs.write_file(a.dst, a.data->data(), a.data->size());
        } else {
            if (over) env.fs.copy_file_replace(a.src, a.dst);
            else env.fs.copy_file(a.src, a.dst);
        }
    }

    // Ledger.put: тот же файл — не трогать (свой — учесть); свой иной — перезаписать начисто;
    // чужой иной — копия .bak- (один раз), потом запись.
    void put(const Action& a) {
        std::string want = sha_of(a);
        if (a.sdk_swap) {   // подмена sdk — своя ветка, как в эталоне
            if (env.fs.is_file(a.dst) && file_sha256(env, a.dst) != want && a.replaces_foreign) {
                std::string bak = a.dst + ".bak-" + plan.stamp;
                std::string prev = file_sha256(env, a.dst);
                env.fs.copy_file(a.dst, bak);
                rep.done.add("копия прежнего: " + bak, "done/backup", {bak});
                swap_foreign = {{"backup", path_name(bak)}, {"sha256", prev}};
            }
            if (!env.fs.is_file(a.dst) || file_sha256(env, a.dst) != want) {
                mkdirs(path_parent(a.dst));
                write(a);
            }
            own(a.dst);
            return;
        }
        if (env.fs.is_file(a.dst)) {
            bool ours = is_ours(a);
            if (file_sha256(env, a.dst) == want) {
                if (ours) own(a.dst);
                rep.done.add("на месте (тот же файл): " + a.dst, "done/same", {a.dst});
                return;
            }
            if (!ours) {
                std::string bak = a.dst + ".bak-" + plan.stamp;
                env.fs.copy_file(a.dst, bak);
                rep.done.add("копия прежнего: " + bak, "done/backup", {bak});
                backed_up(a.dst, bak);
            }
        }
        mkdirs(path_parent(a.dst));
        write(a);
        own(a.dst);
        rep.done.add("записано: " + a.dst, "done/written", {a.dst});
    }

    void guard() {
        if (plan.guard_dir.empty()) return;
        auto after = snapshot_sha(env, plan.guard_dir);
        for (auto& [name, sha] : plan.foreign_before) {
            auto it = after.find(name);
            if (it == after.end()) rep.problems.add("чужой файл " + name + " исчез из exefs назначения", "gate/foreign_gone", {name});
            else if (it->second != sha) rep.problems.add("чужой файл " + name + " изменён", "gate/foreign_changed", {name});
        }
    }

    void verify_puts() {
        for (auto& a : plan.actions) {
            if (a.kind != ActKind::PutFile && a.kind != ActKind::PutBytes) continue;
            std::string want = sha_of(a);
            if (!env.fs.is_file(a.dst)) rep.problems.add("не разложен: " + a.dst, "gate/not_placed", {a.dst});
            else if (file_sha256(env, a.dst) != want) rep.problems.add("лёг НЕ байт в байт: " + a.dst, "gate/not_exact", {a.dst});
        }
    }

    void receipt(const Action& a) {
        verify_puts();
        guard();
        if (!rep.problems.empty())
            throw Error("FAIL_GATE_APPLY", "запись не прошла гейт: " + rep.problems[0] + " — квитанция не записана", {},
                        {rep.problems.msg(0)});
        ojson r = a.receipt;
        r["installed"] = env.clock.iso_seconds();
        if (r.contains("sdk_swap") && r["sdk_swap"].is_object()) {
            ojson& w = r["sdk_swap"];
            if (w.value("installed", "x").empty()) w["installed"] = r["installed"];
            if (!swap_foreign.is_null()) {
                w["foreign_backup"] = swap_foreign["backup"];
                w["foreign_sha256"] = swap_foreign["sha256"];
            }
        }
        const ojson& base = a.receipt_base;
        ojson ow = obj_or_empty(base, "owned");
        for (auto& [k, v] : owned.items()) ow[k] = v;
        // own_backups — только записи прежних квитанций (снятие удалит старые копии по sha256)
        ojson ob = obj_or_empty(base, "own_backups");
        ojson fb = foreign_backups;
        const ojson base_fb = obj_or_empty(base, "foreign_backups");
        for (auto& [k, v] : base_fb.items()) fb[k] = v;   // первый отложенный — подлинный чужой
        std::set<std::string> cd = created;
        if (base.is_object() && base.contains("created_dirs") && base["created_dirs"].is_array())
            for (auto& x : base["created_dirs"]) cd.insert(x.get<std::string>());
        // прежняя квитанция — наш файл: перезаписывается начисто (прежде — копией .bak-)
        // порядок ключей как у json.dumps(dict) эталона: сначала owned..., потом наше installed_by
        r["owned"] = sort_obj(ow);
        r["own_backups"] = sort_obj(ob);
        r["foreign_backups"] = sort_obj(fb);
        r["created_dirs"] = std::vector<std::string>(cd.begin(), cd.end());
        if (!a.installed_by.empty()) r["installed_by"] = a.installed_by;
        mkdirs(path_parent(a.dst));
        std::string text = dump_receipt(r);
        env.fs.write_file_replace(a.dst, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        rep.receipt = r;
        rep.done.add("квитанция: " + a.dst, "done/receipt", {a.dst});
    }

    // dict эталона сохраняет порядок вставки: base, затем новое. Повторяем это, а не сортируем.
    static ojson sort_obj(const ojson& o) { return o; }

    ProgressFn progress;
    void run() {
        bool had_receipt = false;
        size_t idx = 0;
        for (auto& a : plan.actions) {
            if (a.kind == ActKind::PutReceipt && a.receipt_base.is_object() && a.receipt_base.contains("owned") &&
                a.receipt_base["owned"].is_object())
                prior_owned = a.receipt_base["owned"];
        }
        for (auto& a : plan.actions) {
            if (progress) progress(idx, plan.actions.size(), a);
            idx++;
            switch (a.kind) {
            case ActKind::MakeDir: mkdirs(a.dst); break;
            case ActKind::PutFile:
            case ActKind::PutBytes: put(a); break;
            case ActKind::SaveOrig:
                if (env.fs.is_file(a.src) && !env.fs.is_file(a.dst)) {
                    env.fs.copy_file(a.src, a.dst);
                    own(a.dst);
                    rep.done.add("исходный сохранён: " + a.dst, "done/orig_saved", {a.dst});
                }
                break;
            case ActKind::PutReceipt: receipt(a); had_receipt = true; break;
            case ActKind::Delete:
                if (!env.fs.is_file(a.dst)) rep.skipped.add("нет файла (уже убран): " + a.dst, "skipped/gone", {a.dst});
                else if (a.expect_sha256.empty())
                    rep.skipped.add("нет sha256 в квитанции — не удаляем: " + a.dst, "skipped/no_sha", {a.dst});
                else if (file_sha256(env, a.dst) != a.expect_sha256)
                    rep.skipped.add("файл изменён после установки — не удаляем: " + a.dst, "skipped/changed", {a.dst});
                else {
                    env.fs.remove_file(a.dst);
                    rep.done.add("удалено: " + a.dst, "done/deleted", {a.dst});
                }
                break;
            case ActKind::Rename:
                if (env.fs.exists(a.src) && !env.fs.exists(a.dst)) {
                    env.fs.rename(a.src, a.dst);
                    rep.done.add("возвращено: " + a.src + " -> " + a.dst, "done/restored", {a.src, a.dst});
                } else {
                    rep.skipped.add("не возвращено (нет исходного или место занято): " + a.dst, "skipped/not_restored", {a.dst});
                }
                break;
            case ActKind::RemoveEmptyDir:
                if (env.fs.is_dir(a.dst) && env.fs.list_dir(a.dst).empty()) {
                    env.fs.remove_empty_dir(a.dst);
                    rep.done.add("убрана пустая папка: " + a.dst, "done/dir_removed", {a.dst});
                }
                break;
            }
        }
        if (!had_receipt) {
            verify_puts();
            guard();
            if (!rep.problems.empty())
                throw Error("FAIL_GATE_APPLY", "запись не прошла гейт: " + rep.problems[0], {}, {rep.problems.msg(0)});
        }
    }
};

}  // namespace

ApplyReport apply_plan(Env& env, const Plan& plan) { return apply_plan(env, plan, ProgressFn()); }

ApplyReport apply_plan(Env& env, const Plan& plan, const ProgressFn& progress) {
    Exec e(env, plan);
    e.progress = progress;
    e.run();
    return e.rep;
}

}  // namespace mk
