// sdk: опознание, поиск без повторов, порт stubdiff.py (profile/forecast/detect), статус sdk игры,
// подмена sdk игры (exefs/sdk) и её снятие.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/sdk.hpp"
#include "mk/install.hpp"
#include "mk/npdm.hpp"
#include "mk/receipt.hpp"

#include <algorithm>
#include <cstring>
#include <regex>
#include <set>
#include <tuple>

namespace mk {

using oj = nlohmann::ordered_json;

// ------------------------------------------------------------ каталог (тот же X-макрос, что в модуле)
const std::vector<AutofixFix>& autofix_catalog() {
    static const std::vector<AutofixFix> cat = {
#define NXR_FIX(id, sym, kind, scope, hist, own) {#id, sym, #kind, #scope, hist, own},
#ifdef MK_CATALOG_PATH
#include MK_CATALOG_PATH
#else
#include "nxr_autofix_catalog.h"
#endif
#undef NXR_FIX
    };
    return cat;
}

namespace {

const char* kSdkName = "nnSdk";
const char* kVulkan = "vkCreateInstance";

oj vtext(const std::optional<Ver3>& v) { return v ? oj(version_text(*v)) : oj(nullptr); }

std::vector<std::pair<std::string, std::string>> game_files(IFileSystem& fs, const std::string& exefs) {
    std::vector<std::pair<std::string, std::string>> v;
    std::vector<std::string> names{"main"};
    for (int i = 0; i < 10; i++) names.push_back("subsdk" + std::to_string(i));
    for (auto& n : names)
        if (fs.is_file(path_join(exefs, n))) v.emplace_back(n, path_join(exefs, n));
    return v;
}

NsoImage load(Env& env, const std::string& p) { return NsoImage(env.fs.read_file(p), &env.hash); }

bool body_is_stub(const NsoImage& n, uint64_t addr) {
    Bytes b = n.mem(addr, 8);
    if (b.size() < 8) return false;
    return is_return_const_stub(rd32(b.data()), rd32(b.data() + 4));
}

// Импорты модуля игры в общую таблицу (слабый — только если слабый во всех модулях) и его экспорты.
void add_module_symbols(const NsoImage& g, const std::string& nm, std::map<std::string, GameImport>& imps,
                        std::set<std::string>& exports) {
    for (auto& [s, info] : g.imports()) {
        GameImport& gi = imps[s];
        if ((info.info >> 4) != 2) gi.weak = false;   // STB_WEAK = 2
        gi.importers.push_back(nm);
    }
    for (auto& [s, v] : g.exports()) exports.insert(s);
}

// Экспорты sdk -> тело — заглушка; only — только эти символы (nullptr — все).
std::map<std::string, bool> export_stubs(const NsoImage& n, const std::map<std::string, GameImport>* only = nullptr) {
    std::map<std::string, bool> m;
    for (auto& [s, sym] : n.exports())
        if (!only || only->count(s)) m.emplace(s, body_is_stub(n, sym.value));
    return m;
}

bool is_zbic_file(IFileSystem& fs, const std::string& p) {
    if (!fs.is_file(p)) return false;
    Bytes h = fs.read_range(p, 0, 0x10);
    return h.size() >= 0x10 && std::memcmp(h.data(), "NSO0", 4) == 0 && (rd32(h.data() + 0xC) & 0x80);
}

std::pair<std::string, std::string> decide(const AutofixFix& fix, bool sdk_known, bool defined, bool stub,
                                           const std::optional<Ver3>& sv, const std::optional<Ver3>& gv, bool vulkan) {
    if (!defined) return {"own", sdk_known ? "SdkMissing" : "NoSdkMissing"};
    if (!sdk_known) return {"proxy", "NoSdkDefined"};
    if (fix.kind == "MISSING") return {"proxy", "SdkDefined"};
    if (!stub) return {"proxy", "SdkReal"};
    if (fix.scope == "NO_VULKAN" && vulkan) return {"proxy", "StubVulkan"};
    if (!sv || !gv) return {"proxy", "StubNoVersion"};
    if (*sv >= *gv) return {"proxy", "StubNative"};
    return {"own", "StubOlderSdk"};
}

}  // namespace

bool is_return_const_stub(uint32_t i0, uint32_t i1) {
    if (i1 != 0xD65F03C0) return false;
    uint32_t op = i0 & 0xFF80001F;
    return op == 0x52800000 || op == 0xD2800000 || op == 0x12800000 || op == 0x92800000 || i0 == 0x2A1F03E0 ||
           i0 == 0xAA1F03E0;
}

std::string hos_version_text(uint32_t v) {
    return std::to_string((v >> 26) & 0x3F) + "." + std::to_string((v >> 20) & 0x3F) + "." + std::to_string((v >> 16) & 0xF);
}

oj read_cnmt(const Bytes& data, bool xml_hint) {
    size_t i = 0;
    while (i < data.size() && std::isspace(data[i])) i++;
    if (xml_hint || (i < data.size() && data[i] == '<')) {
        std::string s(data.begin(), data.end());
        std::smatch m;
        if (!std::regex_search(s, m, std::regex("<RequiredSystemVersion>\\s*(\\d+)\\s*</RequiredSystemVersion>")))
            return nullptr;
        uint64_t v = std::stoull(m[1].str());
        oj r = oj::object();
        r["required_system_version"] = v;
        r["text"] = hos_version_text(uint32_t(v));
        r["source"] = "xml";
        return r;
    }
    if (data.size() < 0x28) return nullptr;
    uint64_t tid = rd64(data.data());
    uint32_t ver = rd32(data.data() + 8);
    uint8_t type = data[0x0C];
    uint16_t ext = uint16_t(data[0x0E] | data[0x0F] << 8);
    oj r = oj::object();
    r["title_id"] = tid_hex(tid);
    if ((type != 0x80 && type != 0x81) || ext < 0x10) {
        r["type"] = type;
        r["required_system_version"] = nullptr;
        r["text"] = nullptr;
        r["source"] = "cnmt";
        return r;
    }
    uint32_t rsv = rd32(data.data() + 0x28);
    r["version"] = ver;
    r["type"] = type;
    r["required_system_version"] = rsv;
    r["text"] = hos_version_text(rsv);
    r["source"] = "cnmt";
    return r;
}

// ------------------------------------------------------------ опознание и поиск
std::optional<SdkFile> identify_sdk(Env& env, const std::string& path, const std::string& label) {
    if (!env.fs.is_file(path)) return std::nullopt;
    uint64_t size = env.fs.file_size(path);
    if (size < kNsoHeaderSize || size > (96ull << 20)) return std::nullopt;   // nnSdk — единицы МБ
    Bytes head = env.fs.read_range(path, 0, kNsoHeaderSize);
    NsoHeader h;
    if (!parse_nso_header(head.data(), head.size(), h)) return std::nullopt;
    SdkFile f;
    f.path = path;
    f.label = label;
    f.size = size;
    try {
        Bytes raw = env.fs.read_file(path);
        NsoImage n(raw, &env.hash);
        f.module_name = n.module_name().value_or("");
        f.nnsdk = f.module_name == kSdkName;
        f.version = n.mw_version();
        f.module_id = n.header.module_id_hex();
        f.sha256 = sha256_hex(env.hash, raw);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return f;
}

static void walk_files(IFileSystem& fs, const std::string& d, std::vector<std::string>& out, int depth, int max_depth) {
    for (auto& e : fs.list_dir(d)) {
        std::string p = path_join(d, e.name);
        if (e.is_dir) {
            if (depth < max_depth) walk_files(fs, p, out, depth + 1, max_depth);
        } else {
            out.push_back(p);
        }
    }
}

std::vector<SdkEntry> scan_sdk(Env& env, const std::vector<SdkRoot>& roots, int max_depth,
                               std::vector<SdkFile>* rejected) {
    std::vector<SdkEntry> out;
    for (const auto& r : roots) {
        const std::string& root = r.dir;
        const std::string& label = r.label;
        if (!env.fs.is_dir(root)) continue;
        std::vector<std::string> files;
        walk_files(env.fs, root, files, 0, max_depth);
        for (auto& p : files) {
            auto f = identify_sdk(env, p, label);
            if (f && !f->nnsdk && rejected) rejected->push_back(*f);
            if (!f || !f->nnsdk) continue;
            auto it = std::find_if(out.begin(), out.end(), [&](const SdkEntry& e) {
                return e.file.module_id == f->module_id || e.file.sha256 == f->sha256;
            });
            if (it != out.end()) {
                it->sources.emplace_back(label, p);
                continue;
            }
            SdkEntry e;
            e.file = *f;
            e.sources.emplace_back(label, p);
            out.push_back(e);
        }
    }
    for (auto& a : out)
        for (auto& b : out)
            if (&a != &b && a.file.version && a.file.version == b.file.version && a.file.module_id != b.file.module_id)
                a.same_version_other_id = true;
    return out;
}

// ------------------------------------------------------------ stubdiff
oj sdk_profile(Env& env, const std::string& exefs, const std::string* cnmt) {
    oj mods = oj::array();
    oj gfx = {{"vulkan", false}, {"opengl", false}, {"nvn", false}};
    std::vector<std::string> main_imports;
    std::vector<std::string> zfiles;
    for (auto& [nm, p] : game_files(env.fs, exefs)) {
        NsoImage n = load(env, p);
        auto ex = n.exports();
        if (nm == "main")
            for (auto& [s, v] : n.imports()) main_imports.push_back(s);
        bool z = is_zbic_file(env.fs, p);
        oj m = oj::object();
        m["file"] = nm;
        m["name"] = n.module_name() ? oj(*n.module_name()) : oj(nullptr);
        m["mw_version"] = vtext(n.mw_version());
        m["zbic"] = z;
        mods.push_back(m);
        if (z) zfiles.push_back(nm);
        if (ex.count(kVulkan)) gfx["vulkan"] = true;
        if (ex.count("eglCreateWindowSurface")) gfx["opengl"] = true;
    }
    auto any = [&](const char* pre) {
        for (auto& s : main_imports)
            if (s.compare(0, std::strlen(pre), pre) == 0) return true;
        return false;
    };
    oj uses = {{"vulkan", any("vk")}, {"opengl", any("egl")}, {"nvn", any("nvn")}};
    std::string sp = path_join(exefs, "sdk");
    oj out = oj::object();
    out["exefs"] = exefs;
    out["modules"] = mods;
    out["graphics_libraries"] = gfx;
    out["main_imports_graphics"] = uses;
    if (env.fs.is_file(sp)) {
        NsoImage s = load(env, sp);
        out["sdk_version"] = vtext(s.mw_version());
        out["sdk_buildid"] = s.build_id_upper().substr(0, 40);
    } else {
        out["sdk_version"] = nullptr;
        out["sdk_buildid"] = nullptr;
    }
    if (is_zbic_file(env.fs, sp)) zfiles.push_back("sdk");
    out["zbic_files"] = zfiles;
    if (cnmt) {
        Bytes c = env.fs.read_file(*cnmt);
        std::string low = *cnmt;
        for (auto& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        out["cnmt"] = read_cnmt(c, low.size() >= 4 && low.compare(low.size() - 4, 4, ".xml") == 0);
    } else {
        out["cnmt"] = nullptr;
    }
    return out;
}

ForecastGame forecast_game(Env& env, const std::string& exefs) {
    std::set<std::string> cat;
    for (auto& c : autofix_catalog()) cat.insert(c.symbol);
    ForecastGame g;
    std::set<std::string> exports;
    for (auto& [nm, p] : game_files(env.fs, exefs)) {
        NsoImage n = load(env, p);
        auto v = n.mw_version();
        if (v && (!g.version || *v > *g.version)) g.version = v;
        auto ex = n.exports();
        if (!g.vulkan && ex.count(kVulkan)) { g.vulkan = true; g.vulkan_module = nm; }
        if (ex.count("eglCreateWindowSurface")) g.opengl = true;
        for (auto& [s, info] : n.imports())
            if (cat.count(s)) g.imports[s].push_back(nm);
        add_module_symbols(n, nm, g.all_imports, exports);
    }
    for (auto& [s, gi] : g.all_imports)
        if (exports.count(s)) g.module_exports.insert(s);
    // Своя sdk игры — эталон «настоящих» функций для пробелов; не прочиталась — прогноз каталога всё равно считается.
    const std::string sp = path_join(exefs, "sdk");
    try {
        if (!env.fs.is_file(sp)) {
            g.own_sdk_error = "SDK_ABSENT";
        } else {
            NsoImage s = load(env, sp);
            g.own_sdk = export_stubs(s, &g.all_imports);
            g.own_sdk_ok = true;
        }
    } catch (const Error& e) {
        g.own_sdk_error = e.code();
    } catch (const std::exception&) {
        g.own_sdk_error = "SDK_UNREADABLE";
    }
    return g;
}

ForecastSdk forecast_sdk(Env& env, const std::string& sdk_path) {
    NsoImage sdk = load(env, sdk_path);
    ForecastSdk s;
    s.name = sdk.module_name();
    s.known = s.name && *s.name == kSdkName;
    s.version = sdk.mw_version();
    s.exports = export_stubs(sdk);
    return s;
}

std::vector<SdkGap> sdk_gaps(const std::map<std::string, GameImport>& imports, const std::set<std::string>& game_exports,
                             const std::map<std::string, bool>& old_sdk, const std::map<std::string, bool>& own_sdk) {
    std::map<std::string, const AutofixFix*> known;
    for (auto& c : autofix_catalog()) known[c.symbol] = &c;
    std::vector<SdkGap> out;
    for (auto& [sym, gi] : imports) {
        if (game_exports.count(sym)) continue;
        auto ni = own_sdk.find(sym);
        if (ni == own_sdk.end()) continue;
        auto oi = old_sdk.find(sym);
        SdkGap g;
        if (oi == old_sdk.end()) g.kind = "MISSING";
        else if (oi->second && !ni->second) g.kind = "STUB";
        else continue;
        g.symbol = sym;
        g.weak = gi.weak;
        g.importers = gi.importers;
        auto c = known.find(sym);
        if (c != known.end()) g.fix = c->second;
        out.push_back(std::move(g));
    }
    return out;
}

ModuleForecast forecast_fixes(const ForecastGame& game, const ForecastSdk& sdk) {
    ModuleForecast r;
    r.swapped = bool(sdk.version && game.version && *sdk.version < *game.version);
    for (auto& fix : autofix_catalog()) {
        FixForecast f;
        f.fix = fix;
        auto it = sdk.exports.find(fix.symbol);
        f.sdk_has = it != sdk.exports.end();
        f.sdk_stub = f.sdk_has && it->second;
        std::tie(f.choice, f.why) = decide(fix, sdk.known, f.sdk_has, f.sdk_stub, sdk.version, game.version, game.vulkan);
        auto im = game.imports.find(fix.symbol);
        if (im != game.imports.end()) f.importers = im->second;
        r.fixes.push_back(std::move(f));
    }
    return r;
}

// Тот же JSON, что stubdiff.forecast (сверка compare_pc.py); считается через части прогноза выше.
oj sdk_forecast(Env& env, const std::string& console_sdk, const std::string& exefs) {
    const ForecastSdk sdk = forecast_sdk(env, console_sdk);
    const ForecastGame game = forecast_game(env, exefs);
    const ModuleForecast m = forecast_fixes(game, sdk);
    oj fixes = oj::object();
    for (auto& f : m.fixes)
        fixes[f.fix.id] = {{"choice", f.choice}, {"why", f.why}, {"symbol", f.fix.symbol}, {"sdk_has", f.sdk_has},
                           {"sdk_stub", f.sdk_stub}};
    oj r = oj::object();
    r["sdk"] = console_sdk;
    r["sdk_name"] = sdk.name ? oj(*sdk.name) : oj(nullptr);
    r["sdk_known"] = sdk.known;
    r["sdk_version"] = vtext(sdk.version);
    r["game_version"] = vtext(game.version);
    r["swapped"] = m.swapped;
    r["vulkan"] = game.vulkan;
    r["vulkan_module"] = game.vulkan ? oj(game.vulkan_module) : oj(nullptr);
    r["fixes"] = fixes;
    return r;
}

oj sdk_detect(Env& env, const std::string& old_sdk, const std::string& new_sdk, const std::string& exefs) {
    NsoImage o = load(env, old_sdk), n = load(env, new_sdk);
    std::map<std::string, GameImport> imps;
    std::set<std::string> exports;
    for (auto& [nm, p] : game_files(env.fs, exefs)) add_module_symbols(load(env, p), nm, imps, exports);
    oj found = oj::array(), kn = oj::array(), nu = oj::array();
    for (auto& g : sdk_gaps(imps, exports, export_stubs(o, &imps), export_stubs(n, &imps))) {
        bool k = g.fix != nullptr;
        oj f = {{"symbol", g.symbol}, {"kind", g.kind}, {"weak", g.weak}, {"importers", g.importers}, {"known", k},
                {"fix", k ? oj(g.fix->id) : oj(nullptr)}, {"catalog_kind", k ? oj(g.fix->kind) : oj(nullptr)}};
        found.push_back(f);
        (k ? kn : nu).push_back(f);
    }
    oj r = oj::object();
    r["old"] = old_sdk;
    r["new"] = new_sdk;
    r["exefs"] = exefs;
    r["old_version"] = vtext(o.mw_version());
    r["new_version"] = vtext(n.mw_version());
    r["found"] = found;
    r["known"] = kn;
    r["new_unfixed"] = nu;
    return r;
}

// ------------------------------------------------------------ статус sdk игры
const std::vector<FwSdkRow>& fw_sdk_table() {
    // Единственное место таблицы. Правило сравнения «мажор sdk игры <= мажор HOS».
    static const std::vector<FwSdkRow> t = {
        {19, {19, 3, 13}, "проверено на консоли: HOS 19.0.1 + nnSdk 19.3.13"},
        {20, {20, 5, 17}, "правило мажоров: HOS 20 + nnSdk 20.5.17"},
        {22, {22, 2, 0}, "правило мажоров: HOS 22 + nnSdk 22.2.0"},
    };
    return t;
}

SdkStatus evaluate_sdk_status(const SdkStatusInput& in) {
    SdkStatus s;
    s.game_sdk = in.game_sdk_exefs ? in.game_sdk_exefs : in.game_sdk_nca;
    auto set = [&](const char* v, const char* key) {
        s.verdict = v;
        s.text_key = std::string("core/sdk/") + key;
        return s;
    };
    if (!in.firmware) return set("unknown", "unknown_firmware");
    const FwSdkRow* row = nullptr;
    for (auto& r : fw_sdk_table())
        if (r.hos_major <= (*in.firmware)[0] && (!row || r.hos_major > row->hos_major)) row = &r;
    if (row) {
        s.fw_max_sdk = row->sdk;
        s.recommended_source = row->source;
    }
    int fw = (*in.firmware)[0];
    if (in.swapped_present) {
        if (!in.swapped_version) return set("swapped_mismatch", "swapped_unreadable");
        if ((*in.swapped_version)[0] > fw) return set("swapped_mismatch", "swapped_too_new");
        if (s.game_sdk && (*s.game_sdk)[0] <= fw) return set("swapped_mismatch", "swapped_unneeded");
        return set("swapped_ok", "swapped_ok");
    }
    if (!s.game_sdk) return set("unknown", "unknown_game_sdk");
    if ((*s.game_sdk)[0] <= fw) return set("not_needed", "not_needed");
    // нужна подмена: среди найденных — та, что в таблице для этой прошивки, иначе старшая из подходящих
    std::optional<Ver3> best;
    for (auto& v : in.available) {
        if (v[0] > fw) continue;
        if (row && v == row->sdk) { best = v; break; }
        if (!best || v > *best) best = v;
    }
    if (best) {
        s.recommended = best;
        return set("needed", "needed");
    }
    if (row) s.recommended = row->sdk;
    return set("needed", "needed_missing");
}

// ------------------------------------------------------------ подмена sdk
Plan plan_swap_sdk(Env& env, const std::string& dest, const std::string& tid, const SdkFile& sdk,
                   const std::string& installed_by) {
    if (!sdk.nnsdk) throw Error("FAIL_SWAP_SDK_NOT_NNSDK", "выбранный файл не nnSdk: " + sdk.path, {sdk.path});
    Plan plan;
    plan.stamp = env.clock.stamp();
    plan.dest = dest;
    std::string base = title_dir_of(dest, tid), ex = path_join(base, "exefs"), dst = path_join(ex, "sdk");
    std::string T = path_name(base), rel = "atmosphere/contents/" + T + "/exefs/sdk";
    ReceiptRead rec = read_receipt(env, base);
    auto ours = exefs_ours_known(rec);
    auto before = snapshot_sha(env, ex);
    bool foreign = before.count("sdk") && !ours.count("sdk") && before["sdk"] != sdk.sha256;
    oj sb = rec.sane() && rec.data.contains("sdk_swap") && rec.data["sdk_swap"].is_object() ? rec.data["sdk_swap"] : oj::object();
    oj fb = sb.contains("foreign_backup") ? sb["foreign_backup"] : oj(nullptr);
    oj fsha = sb.contains("foreign_sha256") ? sb["foreign_sha256"] : oj(nullptr);
    if (foreign) {
        fb = "sdk.bak-" + plan.stamp;
        fsha = before["sdk"];
        plan.warnings.add("в exefs уже лежит чужой sdk — он будет отложен (" + fb.get<std::string>() +
                              ") и вернётся при снятии", "warn/sdk_foreign_backup", {fb.get<std::string>()});
    }
    plan.guard_dir = ex;
    for (auto& [n, sha] : before)
        if (n != "sdk") plan.foreign_before[n] = sha;
    Action md;
    md.kind = ActKind::MakeDir;
    md.dst = ex;
    md.what = "dir";
    plan.actions.push_back(md);
    Action a;
    a.kind = ActKind::PutFile;
    a.src = sdk.path;
    a.dst = dst;
    a.what = "sdk";
    a.rel = rel;
    a.expect_sha256 = sdk.sha256;
    a.replaces_foreign = foreign;
    plan.actions.push_back(a);
    ReceiptFields rf;
    rf.tid = T;
    if (rec.sane() && rec.data.contains("neighbours"))
        for (auto& x : rec.data["neighbours"]) rf.neighbours.push_back(x.get<std::string>());
    rf.mode = "sdk-swap";
    rf.installed_by = installed_by.empty() ? "console" : installed_by;
    oj w = oj::object();   // формат поля в квитанции формата 1 (SDKSWAP-FORMAT)
    w["file"] = "sdk";
    w["module"] = sdk.module_name;
    w["version"] = sdk.version ? oj(version_text(*sdk.version)) : oj(nullptr);
    w["sha256"] = sdk.sha256;
    w["size"] = sdk.size;
    w["source_name"] = path_name(sdk.path);
    w["foreign_backup"] = fb;
    w["foreign_sha256"] = fsha;
    w["installed"] = "";
    w["verified_on_console"] = false;
    rf.sdk_swap = w;
    Action ra;
    ra.kind = ActKind::PutReceipt;
    ra.dst = path_join(base, kReceiptName);
    ra.what = "receipt";
    ra.receipt = make_receipt(rf, rec);
    // учёт своего прежней квитанции (owned, own_backups, foreign_backups, created_dirs) сливается с этим прогоном,
    // installed_by — в конце (без receipt_base исполнитель записал бы owned только этого прогона)
    ra.receipt_base = rec.sane() ? rec.data : oj::object();
    ra.installed_by = rf.installed_by;
    plan.actions.push_back(ra);
    const std::string sv = sdk.version ? version_text(*sdk.version) : std::string("?");
    plan.lines.add(dst + "   <- " + sdk.path + " (nnSdk " + sv + ")", "plan/sdk_swap", {sv, dst, sdk.path});
    plan.lines.add(ra.dst + "   <- квитанция (sdk_swap)", "plan/put_receipt_sdk", {ra.dst});
    return plan;
}

Plan plan_unswap_sdk(Env& env, const std::string& dest, const std::string& tid) {
    std::string base = title_dir_of(dest, tid), ex = path_join(base, "exefs");
    ReceiptRead rec = read_receipt(env, base);
    if (!rec.sane() || !rec.data.contains("sdk_swap") || !rec.data["sdk_swap"].is_object())
        throw Error("FAIL_UNSWAP_NOTHING", "в квитанции нет подмены sdk — снимать нечего");
    oj modes = rec.data.contains("modes") ? rec.data["modes"] : oj::array();
    bool only_swap = modes.is_array() && modes.size() == 1 && modes[0] == "sdk-swap";
    if (only_swap) return plan_uninstall(env, dest, tid, true);   // кроме подмены ничего не ставили
    Plan plan;
    plan.stamp = env.clock.stamp();
    plan.dest = dest;
    const oj& w = rec.data["sdk_swap"];
    std::string file = w.value("file", "sdk");
    std::string T = path_name(base), rel = "atmosphere/contents/" + T + "/exefs/" + file, dst = path_join(ex, file);
    std::string sha = w.value("sha256", "");
    if (env.fs.is_file(dst)) {
        if (file_sha256(env, dst) != sha) {
            plan.warnings.add("exefs/" + file + " изменён после подмены — не удаляем", "warn/unswap_changed", {file});
        } else {
            Action a;
            a.kind = ActKind::Delete;
            a.dst = dst;
            a.rel = rel;
            a.expect_sha256 = sha;
            a.what = "sdk";
            plan.actions.push_back(a);
            plan.lines.add("удалить " + rel + " (подменённый sdk)", "plan/unswap_delete", {rel});
        }
    }
    oj r = rec.data;
    if (w.contains("foreign_backup") && w["foreign_backup"].is_string() &&
        env.fs.is_file(path_join(ex, w["foreign_backup"].get<std::string>()))) {
        Action a;
        a.kind = ActKind::Rename;
        a.src = path_join(ex, w["foreign_backup"].get<std::string>());
        a.dst = dst;
        a.what = "restore";
        plan.actions.push_back(a);
        plan.lines.add("вернуть прежний sdk: exefs/" + w["foreign_backup"].get<std::string>(), "plan/unswap_restore",
                       {"exefs/" + w["foreign_backup"].get<std::string>()});
    }
    if (r.contains("backups") && r["backups"].is_object()) r["backups"].erase(rel);
    r["sdk_swap"] = nullptr;
    if (r.contains("written") && r["written"].is_object()) r["written"].erase(rel);
    if (r.contains("owned") && r["owned"].is_object()) r["owned"].erase(rel);
    oj m2 = oj::array();
    for (auto& x : modes)
        if (x != "sdk-swap") m2.push_back(x);
    r["modes"] = m2;
    Action ra;
    ra.kind = ActKind::PutReceipt;
    ra.dst = path_join(base, kReceiptName);
    ra.what = "receipt";
    ra.receipt = r;
    ra.receipt_base = r;   // owned без снятого sdk, прочий учёт своего — как был
    plan.actions.push_back(ra);
    plan.lines.add("квитанция: подмена sdk снята (sdk_swap = null)", "plan/unswap_receipt");
    return plan;
}

}  // namespace mk
