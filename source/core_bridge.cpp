// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — мост к ядру (core/include/mk). Всё, что экраны просят у ядра, проходит здесь; mk::Error
// превращается в код и текст ошибки. См. source/core_bridge.hpp.
#include "core_bridge.hpp"

#include <mk/core.hpp>
#include <mk/install.hpp>
#include <mk/logs.hpp>
#include <mk/mod.hpp>
#include <mk/nso.hpp>
#include <mk/plan.hpp>
#include <mk/receipt.hpp>
#include <mk/sdk.hpp>
#include <mk/zbic.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <map>
#include <mutex>
#include <optional>

#include "core_text.hpp"
#include "fixes_view.hpp"
#include "platform/core_fs.hpp"
#include "platform/diag_report.hpp"
#include "platform/exefs.hpp"
#include "platform/file_time.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"

namespace mk::bridge {

namespace {

constexpr const char* kDest = "sdmc:/";
const std::string kManifest = "romfs:/modules/manifest.json";

mk::Env env() { return mk::Env{plat::posixFs(), plat::nxHash(), plat::nxClock()}; }

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string exefsRoot() { return plat::ExefsMount::root(); }

// ---------------- тексты ядра -> переводимые тексты экрана ----------------

app::Text toText(const mk::Msg& m, const std::string& fallback) { return app::coreText(m, fallback); }
app::Text errorText(const mk::Error& ex) { return app::coreError(ex); }
std::vector<app::Text> texts(const mk::Texts& v) { return app::coreTexts(v); }

// ---------------- опись модулей romfs (manifest.json, schema 3) ----------------

struct ModuleChoice {
    mk::ModuleSource main, early;
    std::string mainId, earlyId;
    bool mainDiag = false, mainGfx = false;   // свойства выбранного основного варианта (поля квитанции diag/diag_gfx)
    std::string pipelineVersion = mk::kPipelineVersion;
    std::vector<app::Text> warnings;
    bool ok = false;
};

// Вариант по свойствам из описи: autofix — всегда; diag — для «Отладочной сборки»; early — для мода со своим
// модулем; gfx (графическая диагностика) — не по умолчанию. Имена вариантов задаёт агент модулей.
const nlohmann::json* findVariant(const nlohmann::json& mods, bool diag, bool early) {
    for (const auto& m : mods) {
        if (!m.is_object()) continue;
        if (m.value("autofix", false) && m.value("diag", false) == diag && m.value("early", false) == early &&
            !m.value("gfx", false) && !m.value("stale", false)) {
            return &m;
        }
    }
    return nullptr;
}

ModuleChoice chooseModules(bool debug) {
    ModuleChoice c;
    nlohmann::json man;
    try {
        const mk::Bytes raw = plat::posixFs().read_file(kManifest);
        man = nlohmann::json::parse(raw.begin(), raw.end());
    } catch (const std::exception& e) {
        MK_LOG("bridge: manifest %s: %s", kManifest.c_str(), e.what());
        return c;
    }
    const auto& mods = man.value("modules", nlohmann::json::array());
    c.pipelineVersion = man.value("pipeline_version", std::string(mk::kPipelineVersion));
    auto fill = [](const nlohmann::json* m, mk::ModuleSource& src, std::string& id) {
        if (!m) return false;
        src.path = std::string("romfs:/modules/") + m->value("file", "");
        src.expect_sha256 = m->value("sha256", "");
        id = m->value("id", "");
        return true;
    };
    const nlohmann::json* main = findVariant(mods, debug, false);
    const nlohmann::json* early = findVariant(mods, debug, true);
    if (debug && !main) {
        app::Text t = app::textOf("app/op/no_debug_module");
        t.fallback = "в приложении нет отладочного модуля — ставится обычный";
        c.warnings.push_back(std::move(t));
        main = findVariant(mods, false, false);
    }
    if (debug && !early) early = findVariant(mods, false, true);
    c.ok = fill(main, c.main, c.mainId);
    if (main) {
        c.mainDiag = main->value("diag", false);
        c.mainGfx = main->value("gfx", false);
    }
    fill(early, c.early, c.earlyId);
    MK_LOG("bridge: modules main=%s early=%s debug=%d", c.mainId.c_str(), c.earlyId.c_str(), debug);
    return c;
}

// ---------------- последний план ----------------

std::mutex g_mtx;
struct Pending {
    bool have = false;
    uint64_t tid = 0;
    mk::Plan plan;
    std::optional<mk::SdkFile> swapAfter;   // подмена sdk после установки — план строится заново после записи
};
Pending g_pending;

app::PlanView errorView(app::OpKind op, const std::string& code, app::Text text) {
    app::PlanView v;
    v.op = op;
    v.ok = false;
    v.errorCode = code;
    v.error = std::move(text);
    return v;
}

// Ошибка приложения (не ядра): ключ app/error/<код>.
app::PlanView appError(app::OpKind op, const std::string& code) {
    return errorView(op, code, app::textOf("app/error/" + code));
}

void addLines(app::PlanView& v, const mk::Plan& p) {
    for (auto& t : texts(p.lines)) v.items.push_back(app::PlanItem{std::move(t)});
    for (auto& t : texts(p.warnings)) {
        // Модуль в romfs всегда лежит как «subsdk» и кладётся под выбранным именем — предупреждение эталона
        // («имя файла ≠ слот») на консоли ничего не сообщает.
        if (t.key == "core/warn/module_name_slot") continue;
        v.warnings.push_back(std::move(t));
    }
}

}  // namespace

bool identifySdk(const std::string& path, SdkProbe& out) {
    out = SdkProbe{};
    try {
        mk::Env e = env();
        if (!e.fs.is_file(path)) return false;
        const auto f = mk::identify_sdk(e, path, "probe");
        if (!f) return false;
        out.isNso = true;
        out.isNnSdk = f->nnsdk;
        out.moduleName = f->module_name;
        if (f->version) out.mwVersion = mk::version_text(*f->version);
        return true;
    } catch (const mk::Error& e) {
        MK_LOG("bridge: identify %s: %s %s", path.c_str(), e.code().c_str(), e.what());
        return false;
    }
}

bool inspectGame(const GameCtx& g, app::GameDetails& d) {
    mk::Env e = env();
    bool ok = true;
    const std::string titleDir = mk::title_dir_of(kDest, g.tid);

    // Квитанция и наше в папке игры.
    try {
        const mk::InstalledState st = mk::inspect_installed(e, kDest, g.tid);
        d.ourInstalled = st.receipt.sane();
        if (d.ourInstalled) {
            const auto& r = st.receipt.data;
            d.slot = r.value("slot", "");
            d.installedAt = r.value("installed", "");
            d.installedBy = r.value("installed_by", "");
            d.sdk.swapOurs = r.contains("sdk_swap") && !r["sdk_swap"].is_null();
            // Вид сборки: по квитанции (поля формата 1 diag, diag_gfx, module_variant; в прежних — только diag).
            auto flag = [&r](const char* k) { return r.contains(k) && r[k].is_boolean() && r[k].get<bool>(); };
            d.moduleVariant = r.contains("module_variant") && r["module_variant"].is_string()
                                  ? r["module_variant"].get<std::string>() : "";
            d.debugInstalled = flag("diag") || flag("diag_gfx") || d.moduleVariant.find("diag") != std::string::npos ||
                               d.moduleVariant.find("gfx") != std::string::npos;
        }
        if (st.receipt.broken) {
            app::Text t = app::textOf("app/state/receipt_broken");
            t.fallback = "квитанция повреждена: " + st.receipt.broken_why;
            d.notes.push_back(std::move(t));
        }
        d.receiptStale = st.module_matches.has_value() && !*st.module_matches;
        // ZBIC уже снят нами: все файлы zbic_files квитанции лежат в exefs папки игры и собраны из этой версии игры.
        if (d.ourInstalled && g.exefsOpen) {
            const auto& r = st.receipt.data;
            if (r.contains("zbic_files") && r["zbic_files"].is_array() && !r["zbic_files"].empty()) {
                bool all = true;
                for (const auto& z : r["zbic_files"])
                    all &= z.is_string() && e.fs.is_file(mk::path_join(titleDir + "/exefs", z.get<std::string>()));
                const auto same = all ? mk::installed_matches_game(e, kDest, g.tid, exefsRoot()) : std::nullopt;
                d.zbicUnpackedOurs = all && same.value_or(true);
            }
        }
    } catch (const mk::Error& ex) {
        ok = false;
        d.coreError = errorText(ex);
        MK_LOG("bridge: inspect_installed %s: %s", ex.code().c_str(), ex.what());
    }

    // ZBIC и собственная sdk игры — по exefs.
    if (g.exefsOpen) {
        try {
            const mk::ZbicScan z = mk::zbic_scan(e.fs, exefsRoot());
            d.zbic = z.zbic();
            d.zbicFiles = z.zbic_files;
        } catch (const mk::Error& ex) {
            ok = false;
            d.coreError = errorText(ex);
        }
        SdkProbe own;
        if (identifySdk(exefsRoot() + "sdk", own) && own.isNnSdk) d.sdk.exefsSdk = own.mwVersion;
    } else {
        ok = false;
    }

    // Лежащий exefs/sdk (подмена).
    const std::string swapPath = titleDir + "/exefs/sdk";
    d.sdk.swapPresent = e.fs.is_file(swapPath);
    if (d.sdk.swapPresent) {
        SdkProbe p;
        d.sdk.swapIsNnSdk = identifySdk(swapPath, p) && p.isNnSdk;
        d.sdk.swapVersion = p.mwVersion;
    }
    d.coreOk = ok;
    return ok;
}

void forecastFixes(const GameCtx& g, const std::string& currentSdkPath, bool currentIsSwap,
                   const std::vector<std::pair<std::string, std::string>>& candidates, app::GameDetails& d) {
    static std::mutex cacheMtx;
    static std::map<std::string, mk::ForecastSdk> cache;   // путь + "|" + sha256 -> сторона sdk
    mk::Env e = env();
    app::FixesBlock& b = d.fixes;
    b = app::FixesBlock{};
    b.currentIsSwap = currentIsSwap;
    b.bySdk.resize(candidates.size());
    if (!g.exefsOpen) return;
    mk::ForecastGame game;
    try {
        game = mk::forecast_game(e, exefsRoot());
        app::fillFixesGame(b, game);
    } catch (const mk::Error& ex) {
        b.error = errorText(ex);
        MK_LOG("bridge: forecast_game %s: %s", ex.code().c_str(), ex.what());
        return;
    }
    auto one = [&](const std::string& path, const std::string& key) {
        app::FixForecastView v;
        try {
            mk::ForecastSdk s;
            bool have = false;
            if (!key.empty()) {
                std::lock_guard<std::mutex> lock(cacheMtx);
                auto it = cache.find(key);
                if (it != cache.end()) { s = it->second; have = true; }
            }
            if (!have) {
                s = mk::forecast_sdk(e, path);
                if (!key.empty()) {
                    std::lock_guard<std::mutex> lock(cacheMtx);
                    cache[key] = s;
                }
            }
            v = app::fixesView(game, s);
        } catch (const mk::Error& ex) {
            v.error = errorText(ex);
            MK_LOG("bridge: forecast_sdk %s: %s %s", path.c_str(), ex.code().c_str(), ex.what());
        }
        return v;
    };
    if (!currentSdkPath.empty() && e.fs.is_file(currentSdkPath)) b.current = one(currentSdkPath, "");
    for (size_t i = 0; i < candidates.size(); ++i)
        b.bySdk[i] = one(candidates[i].first, candidates[i].second.empty() ? "" : candidates[i].first + "|" + candidates[i].second);
}

std::vector<app::ModLine> findMods(const GameCtx& g) {
    std::vector<app::ModLine> out;
    mk::Env e = env();
    const std::string tid = upper(g.tid);
    try {
        if (e.fs.is_dir(plat::paths::kModsDir)) {
            for (const auto& de : e.fs.list_dir(plat::paths::kModsDir)) {
                if (!de.is_dir) continue;
                const std::string path = mk::path_join(plat::paths::kModsDir, de.name);
                const auto tree = mk::find_mod_tree(e.fs, path);
                const bool byName = upper(de.name) == tid;
                if (!(tree && tree->titles.count(tid)) && !byName) continue;
                app::ModLine l;
                l.id = de.name;
                l.title = de.name;
                l.path = path;
                if (tree && tree->titles.count(tid)) {
                    const std::string td = tree->titles.at(tid);
                    l.hasRomfs = e.fs.is_dir(mk::path_join(td, "romfs"));
                    const std::string ex = mk::path_join(td, "exefs");
                    if (e.fs.is_dir(ex)) {
                        for (const auto& f : e.fs.list_dir(ex))
                            if (!f.is_dir && f.name.rfind("subsdk", 0) == 0) l.hasOwnModule = true;
                    }
                    if (tree->atmosphere) l.hasIps = e.fs.is_dir(mk::path_join(*tree->atmosphere, "exefs_patches"));
                }
                out.push_back(std::move(l));
            }
        }
        // Уже лежит в atmosphere/contents/<TID> (поставлено раньше, не нами) — показываем для сведения.
        const std::string td = mk::title_dir_of(kDest, g.tid);
        if (e.fs.is_dir(mk::path_join(td, "romfs")) && !e.fs.is_file(mk::path_join(td, mk::kReceiptName))) {
            app::ModLine l;
            l.id = "contents";
            l.title = "atmosphere/contents/" + g.tid;
            l.path = td;
            l.inContents = true;
            l.hasRomfs = true;
            out.push_back(std::move(l));
        }
    } catch (const mk::Error& ex) {
        MK_LOG("bridge: findMods %s: %s", ex.code().c_str(), ex.what());
    }
    return out;
}

app::PlanView makePlan(app::OpKind op, const GameCtx& g, const std::string& modPath, const std::string& sdkPath,
                       bool unpackZbic, bool debugBuild) {
    mk::Env e = env();
    app::PlanView v;
    v.op = op;
    Pending next;
    next.tid = g.titleId;
    try {
        switch (op) {
            case app::OpKind::Install: {
                if (!g.exefsOpen) return appError(op, "no_exefs");
                const ModuleChoice mods = chooseModules(debugBuild);
                if (!mods.ok) return appError(op, "module_manifest_bad");
                mk::InstallOptions opt;
                opt.exefs = exefsRoot();
                opt.dest = kDest;
                opt.tid = g.tid;
                opt.module = mods.main;
                opt.module_early = mods.early;
                opt.mod = modPath;
                opt.zbic_fix = unpackZbic;
                opt.swapped_sdk = !sdkPath.empty() || e.fs.is_file(mk::title_dir_of(kDest, g.tid) + "/exefs/sdk");
                opt.installed_by = "console";
                opt.pipeline_version = mods.pipelineVersion;
                // Квитанция называет вид сборки (diag, diag_gfx, module_variant) — по свойствам поставленного варианта.
                opt.diag = mods.mainDiag;
                opt.diag_gfx = mods.mainGfx;
                const mk::InstallPlan p = mk::plan_install(e, opt);
                addLines(v, p);
                for (const auto& w : mods.warnings) v.warnings.push_back(w);
                next.plan = p;
                if (!sdkPath.empty()) {
                    // Подмена sdk вместе с установкой: показываем её план сейчас, исполняем — заново после записи
                    // установки (квитанция дописывается поверх уже записанной).
                    const auto f = mk::identify_sdk(e, sdkPath, "modkit");
                    if (!f || !f->nnsdk) return appError(op, "sdk_not_nnsdk");
                    const mk::Plan sp = mk::plan_swap_sdk(e, kDest, g.tid, *f);
                    addLines(v, sp);
                    next.swapAfter = *f;
                }
                break;
            }
            case app::OpKind::Uninstall: {
                const mk::Plan p = mk::plan_uninstall(e, kDest, g.tid);
                addLines(v, p);
                next.plan = p;
                break;
            }
            case app::OpKind::SdkSwap: {
                const auto f = mk::identify_sdk(e, sdkPath, "modkit");
                if (!f || !f->nnsdk) return appError(op, "sdk_not_nnsdk");
                const mk::Plan p = mk::plan_swap_sdk(e, kDest, g.tid, *f);
                addLines(v, p);
                next.plan = p;
                break;
            }
            case app::OpKind::SdkUnswap: {
                const mk::Plan p = mk::plan_unswap_sdk(e, kDest, g.tid);
                addLines(v, p);
                next.plan = p;
                break;
            }
        }
    } catch (const mk::Error& ex) {
        MK_LOG("bridge: plan op=%d %s: %s", static_cast<int>(op), ex.code().c_str(), ex.what());
        return errorView(op, ex.code(), errorText(ex));
    }
    v.ok = true;
    next.have = true;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_pending = std::move(next);
    }
    MK_LOG("bridge: plan op=%d tid=%s lines=%zu warnings=%zu", static_cast<int>(op), g.tid.c_str(), v.items.size(),
           v.warnings.size());
    return v;
}

app::ApplyView applyPending(const GameCtx& g, const ProgressFn& progress) {
    app::ApplyView v;
    v.logPath = plat::log::currentPath();
    Pending p;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (!g_pending.have || g_pending.tid != g.titleId) {
            v.error = app::textOf("app/error/no_pending_plan");
            return v;
        }
        p = std::move(g_pending);
        g_pending = Pending{};  // план исполняется один раз
    }
    mk::Env e = env();
    const size_t total = p.swapAfter ? 2 : 1;
    // Шаг прогресса — первая строка плана, которая что-то кладёт или удаляет (не строка о выборе места).
    auto step = [&](size_t i, const mk::Plan& pl) {
        if (!progress || pl.lines.empty()) return;
        size_t k = 0;
        for (size_t j = 0; j < pl.lines.size(); ++j) {
            const std::string key = pl.lines.msg(j).key;
            if (key.rfind("plan/put_", 0) == 0 || key == "plan/sdk_swap" || key.rfind("plan/un", 0) == 0) {
                k = j;
                break;
            }
        }
        progress(i, total, app::PlanItem{toText(pl.lines.msg(k), pl.lines[k])});
    };
    auto take = [&v](const mk::ApplyReport& r) {
        // Квитанция пишется и установкой, и подменой sdk после неё: на экране итога — одна строка «Квитанция».
        auto hasReceipt = [&v] {
            for (const auto& d : v.done)
                if (d.key == "core/done/receipt") return true;
            return false;
        };
        for (auto& t : texts(r.done)) {
            if (t.key == "core/done/receipt" && hasReceipt()) continue;
            v.done.push_back(std::move(t));
        }
        for (auto& t : texts(r.skipped)) v.skipped.push_back(std::move(t));
        for (auto& t : texts(r.problems)) v.problems.push_back(std::move(t));
    };
    try {
        step(0, p.plan);
        take(mk::apply_plan(e, p.plan));
        if (p.swapAfter) {
            const mk::Plan sp = mk::plan_swap_sdk(e, kDest, g.tid, *p.swapAfter);
            step(1, sp);
            take(mk::apply_plan(e, sp));
        }
        v.ok = true;
    } catch (const mk::Error& ex) {
        v.ok = false;
        v.error = errorText(ex);
        MK_LOG("bridge: apply %s: %s", ex.code().c_str(), ex.what());
    }
    const std::string receipt = mk::path_join(mk::title_dir_of(kDest, g.tid), mk::kReceiptName);
    if (e.fs.is_file(receipt)) v.receiptPath = receipt;
    MK_LOG("bridge: apply tid=%s ok=%d lines=%zu", g.tid.c_str(), v.ok, v.done.size());
    return v;
}

app::LogsView collectLogs(const GameCtx& g, const std::string& gameName, const std::string& passportText, bool write,
                          const std::vector<std::pair<std::string, std::string>>& appTexts) {
    app::LogsView v;
    mk::Env e = env();
    try {
        mk::logs::BundleInput in;
        in.tid = upper(g.tid);
        in.game_name = gameName;
        in.game_version = g.displayVersion;
        in.logs_root = plat::paths::kLogsDir;
        in.crash_dir = plat::paths::kCrashReportsDir;
        in.contents_dir = plat::paths::kContentsDir;
        in.legacy_diag = plat::diag::kDiagPath;
        in.legacy_gfx = plat::diag::kGfxPath;
        in.passport_text = passportText;
        in.app_texts = appTexts;
        in.stamp = e.clock.stamp();
        const mk::logs::Bundle b = mk::logs::plan_logs_bundle(e, in, plat::fileMtime);
        for (auto& t : texts(b.lines)) v.items.push_back(std::move(t));
        for (auto& t : texts(b.warnings)) v.warnings.push_back(std::move(t));
        v.folder = b.folder;
        v.zip = b.zip;
        v.files = b.items.size();
        if (write) {
            const mk::logs::BundleResult r = mk::logs::write_logs_bundle(e, b);
            v.files = r.files;
        }
        v.ok = true;
    } catch (const mk::Error& ex) {
        v.ok = false;
        v.error = errorText(ex);
        MK_LOG("bridge: logs %s: %s", ex.code().c_str(), ex.what());
    }
    return v;
}

}  // namespace mk::bridge
