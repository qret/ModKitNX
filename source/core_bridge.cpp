// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — мост к ядру (core/include/mk). Всё, что экраны просят у ядра, проходит здесь; mk::Error
// превращается в код и текст ошибки. См. source/core_bridge.hpp.
#include "core_bridge.hpp"

#include <mk/cache.hpp>
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
#include "platform/timing.hpp"

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
        out.sha256 = f->sha256;
        if (f->version) out.mwVersion = mk::version_text(*f->version);
        return true;
    } catch (const mk::Error& e) {
        MK_LOG("bridge: identify %s: %s %s", path.c_str(), e.code().c_str(), e.what());
        return false;
    }
}

namespace {

std::string checkPathFor(const std::string& tid) { return plat::paths::kGameCheckCacheDir + upper(tid) + ".json"; }

// «Чем открыта exefs» — для «Технических подробностей» при показе из кэша.
nlohmann::json exefsToJson(const plat::ExefsReport& r) {
    return nlohmann::json{{"via", static_cast<int>(r.via)},  {"rc_with_id", r.rcWithId},
                          {"rc_with_patch", r.rcWithPatch},   {"keys", r.keysPresent},
                          {"main_npdm", r.hasMainNpdm},       {"raw_tried", r.rawTried},
                          {"raw_step", r.rawStep},            {"rc_raw", r.rcRaw},
                          {"raw_core", r.rawCore},            {"raw_rights_id", r.rawRightsId},
                          {"raw_personalized", r.rawPersonalized}};
}
void exefsFromJson(const nlohmann::json& j, plat::ExefsReport& r) {
    r = plat::ExefsReport{};
    try {
        const int via = j.at("via").get<int>();
        if (via >= 0 && via <= static_cast<int>(plat::ExefsVia::RawNcaWithKeys)) r.via = static_cast<plat::ExefsVia>(via);
        r.rcWithId = j.at("rc_with_id").get<uint32_t>();
        r.rcWithPatch = j.at("rc_with_patch").get<uint32_t>();
        r.keysPresent = j.at("keys").get<bool>();
        r.hasMainNpdm = j.at("main_npdm").get<bool>();
        r.rawTried = j.at("raw_tried").get<bool>();
        r.rawStep = j.at("raw_step").get<std::string>();
        r.rcRaw = j.at("rc_raw").get<uint32_t>();
        r.rawCore = j.at("raw_core").get<std::string>();
        r.rawRightsId = j.at("raw_rights_id").get<bool>();
        r.rawPersonalized = j.at("raw_personalized").get<bool>();
    } catch (const std::exception&) {
        // сведения для подробностей; неполные — показываем, что есть
    }
}

// Квитанция и наше в папке игры — всегда живьём (это состояние меняет пользователь и наши операции).
// ZBIC уже снят нами — по ModuleId игры: из кэша (ids) или по смонтированной exefs.
bool inspectReceipt(mk::Env& e, const GameCtx& g, app::GameDetails& d, const std::map<std::string, std::string>* ids) {
    const std::string titleDir = mk::title_dir_of(kDest, g.tid);
    try {
        const mk::InstalledState st = mk::inspect_installed(e, kDest, g.tid);
        d.ourInstalled = st.receipt.sane();
        if (d.ourInstalled) {
            const auto& r = st.receipt.data;
            // Поле может быть null (квитанция одной подмены sdk: слота модуля нет) — value() бросил бы исключение.
            auto str = [&r](const char* k) { return r.contains(k) && r[k].is_string() ? r[k].get<std::string>() : std::string(); };
            d.slot = str("slot");
            d.installedAt = str("installed");
            d.installedBy = str("installed_by");
            d.sdk.swapOurs = r.contains("sdk_swap") && !r["sdk_swap"].is_null();
            if (d.sdk.swapOurs && r["sdk_swap"].is_object() && r["sdk_swap"].contains("sha256") &&
                r["sdk_swap"]["sha256"].is_string())
                d.installedSdkSha = r["sdk_swap"]["sha256"].get<std::string>();
            if (r.contains("mod") && r["mod"].is_object() && r["mod"].contains("source") && r["mod"]["source"].is_string())
                d.installedModPath = r["mod"]["source"].get<std::string>();
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
        if (d.ourInstalled && (ids || g.exefsOpen)) {
            const auto& r = st.receipt.data;
            if (r.contains("zbic_files") && r["zbic_files"].is_array() && !r["zbic_files"].empty()) {
                bool all = true;
                for (const auto& z : r["zbic_files"])
                    all &= z.is_string() && e.fs.is_file(mk::path_join(titleDir + "/exefs", z.get<std::string>()));
                std::optional<bool> same;
                if (all) same = ids ? mk::installed_matches_game(e, kDest, g.tid, *ids)
                                    : mk::installed_matches_game(e, kDest, g.tid, exefsRoot());
                d.zbicUnpackedOurs = all && same.value_or(true);
            }
        }
        return true;
    } catch (const mk::Error& ex) {
        d.coreError = errorText(ex);
        MK_LOG("bridge: inspect_installed %s: %s", ex.code().c_str(), ex.what());
        return false;
    }
}

// Стороны sdk в памяти — по sha256 содержимого (тот же файл в своей папке и в DBI — один разбор; переименование
// не мешает). Полные: урезаются под игру при выдаче.
std::mutex g_sideMtx;
std::map<std::string, mk::ForecastSdk> g_sides;

}  // namespace

void clearMemoryCache() {
    std::lock_guard<std::mutex> lock(g_sideMtx);
    g_sides.clear();
}

bool dropGameCheck(const std::string& tid) {
    mk::Env e = env();
    const std::string p = checkPathFor(tid);
    try {
        if (!e.fs.is_file(p)) return true;
        e.fs.remove_file(p);   // файл кэша приложения (не данные пользователя): проверится заново
        return true;
    } catch (const std::exception& ex) {
        MK_LOG("bridge: game check cache %s not dropped: %s", p.c_str(), ex.what());
        return false;
    }
}

void checkGame(const GameCtx& g0, const CheckKeys& k, const std::function<bool(plat::ExefsReport&)>& openExefs,
               const std::string& swapPath, const std::string& swapSha,
               const std::vector<std::pair<std::string, std::string>>& candidates, app::GameDetails& d) {
    namespace C = mk::cache;
    const uint64_t t0 = plat::nowMs();
    mk::Env e = env();
    GameCtx g = g0;
    const std::string app = C::app_key(MK_APP_VERSION);
    const std::string cpath = checkPathFor(g.tid);
    std::optional<C::GameCheck> c;
    if (k.useCache && !k.key.empty()) c = C::parse_game_check(C::read_cache_file(e, cpath), app, k.key, k.fw, k.ams);

    bool zbicOk = false, gameOk = false, cacheable = false, dirty = false;
    app::FixesBlock& b = d.fixes;
    b = app::FixesBlock{};
    b.currentIsSwap = !swapPath.empty();
    b.bySdk.resize(candidates.size());
    if (c) {
        d.checkFromCache = true;
        d.exefsOpened = true;
        exefsFromJson(c->exefs, d.exefs);
        zbicOk = gameOk = true;
    } else {
        const uint64_t t1 = plat::nowMs();
        d.exefsOpened = openExefs && openExefs(d.exefs);
        g.exefsOpen = d.exefsOpened;
        MK_LOG("model: step exefs open %s ok=%d %llu ms", g.tid.c_str(), d.exefsOpened, plat::msSince(t1));
        if (d.exefsOpened) {
            C::GameCheck n;
            n.key = k.key;
            n.fw = k.fw;
            n.ams = k.ams;
            n.exefs = exefsToJson(d.exefs);
            try {
                const mk::ZbicScan z = mk::zbic_scan(e.fs, exefsRoot());
                n.zbic = z.zbic();
                n.zbic_files = z.zbic_files;
                n.module_ids = mk::exefs_module_ids(e, exefsRoot());
                zbicOk = true;
            } catch (const mk::Error& ex) {
                d.coreError = errorText(ex);
                MK_LOG("bridge: zbic_scan %s: %s", ex.code().c_str(), ex.what());
            }
            const uint64_t t2 = plat::nowMs();
            try {
                mk::ForecastSdk own;
                const mk::ForecastGame full = mk::forecast_game(e, exefsRoot(), &own);
                if (!full.own_sdk_ok) MK_LOG("bridge: sdk gaps: own sdk not read (%s) - not checked", full.own_sdk_error.c_str());
                n.game = C::compact_game(full);   // и живьём, и из кэша — один и тот же урезанный вид
                if (full.own_sdk_ok) n.own_side = C::restrict_sdk(own, n.game);
                gameOk = true;
                // Временный сбой чтения своей sdk (не «её нет») — в кэш не пишем, проверим в следующий раз.
                cacheable = zbicOk && (full.own_sdk_ok || full.own_sdk_error == "SDK_ABSENT");
            } catch (const mk::Error& ex) {
                b.error = errorText(ex);
                MK_LOG("bridge: forecast_game %s: %s", ex.code().c_str(), ex.what());
            }
            MK_LOG("model: step forecast_game %s %llu ms", g.tid.c_str(), plat::msSince(t2));
            c = std::move(n);
        }
    }

    const bool receiptOk = inspectReceipt(e, g, d, (c && !g.exefsOpen) ? &c->module_ids : nullptr);
    if (c && zbicOk) {
        d.zbic = c->zbic;
        d.zbicFiles = c->zbic_files;
    }
    if (c && c->own_side && c->own_side->known && c->own_side->version)
        d.sdk.exefsSdk = mk::version_text(*c->own_side->version);
    d.coreOk = receiptOk && zbicOk;

    if (c && gameOk) {
        const mk::ForecastGame& game = c->game;
        app::fillFixesGame(b, game);
        // Сторона sdk: кэш игры (by_sdk по sha256) → память → файл. Ошибка — в строку прогноза, не в кэш.
        auto side = [&](const std::string& path, const std::string& sha) -> app::FixForecastView {
            app::FixForecastView v;
            try {
                std::optional<mk::ForecastSdk> s;
                if (!sha.empty()) {
                    auto it = c->by_sdk.find(sha);
                    if (it != c->by_sdk.end()) s = it->second;
                }
                if (!s) {
                    mk::ForecastSdk full;
                    bool have = false;
                    if (!sha.empty()) {
                        std::lock_guard<std::mutex> lock(g_sideMtx);
                        auto it = g_sides.find(sha);
                        if (it != g_sides.end()) {
                            full = it->second;
                            have = true;
                        }
                    }
                    if (!have) {
                        const uint64_t t3 = plat::nowMs();
                        full = mk::forecast_sdk(e, path);
                        MK_LOG("model: step forecast_sdk %s %llu ms", path.c_str(), plat::msSince(t3));
                        if (!sha.empty()) {
                            std::lock_guard<std::mutex> lock(g_sideMtx);
                            g_sides[sha] = full;
                        }
                    }
                    s = C::restrict_sdk(full, game);
                    if (!sha.empty()) {
                        c->by_sdk[sha] = *s;
                        dirty = true;
                    }
                }
                v = app::fixesView(game, *s);
                // Полный список пробелов — в журнал приложения (для разработчика: что добавить в каталог).
                if (!v.gaps.empty()) MK_LOG("bridge: sdk gaps %s: %zu not covered by the module", path.c_str(), v.gaps.size());
                for (const auto& gp : v.gaps) {
                    std::string who;
                    for (const auto& m : gp.importers) who += (who.empty() ? "" : ",") + m;
                    MK_LOG("bridge:   %s %s%s [%s]", gp.kind.c_str(), gp.symbol.c_str(), gp.weak ? " (weak)" : "", who.c_str());
                }
            } catch (const mk::Error& ex) {
                v.error = errorText(ex);
                MK_LOG("bridge: forecast_sdk %s: %s %s", path.c_str(), ex.code().c_str(), ex.what());
            }
            return v;
        };
        if (!swapPath.empty()) {
            if (e.fs.is_file(swapPath)) b.current = side(swapPath, swapSha);
        } else if (c->own_side) {
            b.current = app::fixesView(game, *c->own_side);
        }
        for (size_t i = 0; i < candidates.size(); ++i) b.bySdk[i] = side(candidates[i].first, candidates[i].second);
    }

    // Записать: свежая проверка без ошибок, или из кэша дописаны новые стороны sdk. Один файл на игру.
    if (c && !k.key.empty() && ((d.checkFromCache && dirty) || (!d.checkFromCache && cacheable))) {
        try {
            C::write_cache_file(e, cpath, C::dump_game_check(*c, app));
        } catch (const std::exception& ex) {
            MK_LOG("bridge: game check cache not saved: %s", ex.what());
        }
    }
    MK_LOG("model: step check %s cache=%s %llu ms", g.tid.c_str(), d.checkFromCache ? "hit" : (k.useCache ? "miss" : "bypass"),
           plat::msSince(t0));
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
