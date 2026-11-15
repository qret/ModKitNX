// Кэш показа: отпечатки файлов sdk, список игр, проверка игры. См. mk/cache.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/cache.hpp"

#include <cctype>
#include <cstdio>

namespace mk::cache {

using json = nlohmann::json;

namespace {

// ---------------- мелкие разборщики: неверный тип -> исключение json (ловит parse_* и отдаёт промах)

std::string str(const json& j, const char* k) { return j.at(k).get<std::string>(); }
bool flag(const json& j, const char* k) { return j.at(k).get<bool>(); }
std::vector<std::string> strs(const json& j) {
    std::vector<std::string> v;
    for (const auto& e : j) v.push_back(e.get<std::string>());
    return v;
}
bool is_hex(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::optional<Ver3> ver_of(const json& j) {
    if (j.is_null()) return std::nullopt;
    const std::string s = j.get<std::string>();
    Ver3 v{};
    if (std::sscanf(s.c_str(), "%d.%d.%d", &v[0], &v[1], &v[2]) != 3) throw std::runtime_error("bad version");
    return v;
}
json ver_json(const std::optional<Ver3>& v) { return v ? json(version_text(*v)) : json(nullptr); }

json stamp_json(const FileStamp& s) {
    json j = json::object();
    j["size"] = s.size;
    j["mtime"] = s.mtime ? json(*s.mtime) : json(nullptr);
    j["head"] = s.head;
    return j;
}
FileStamp stamp_of(const json& j) {
    FileStamp s;
    if (!j.at("size").is_number_unsigned()) throw std::runtime_error("bad size");
    s.size = j.at("size").get<uint64_t>();
    const json& m = j.at("mtime");
    if (!m.is_null()) {
        if (!m.is_number_integer()) throw std::runtime_error("bad mtime");
        s.mtime = m.get<int64_t>();
    }
    s.head = str(j, "head");
    if (!is_hex(s.head, 64)) throw std::runtime_error("bad head");
    return s;
}

json sdkfile_json(const SdkFile& f) {
    json j = json::object();
    j["nnsdk"] = f.nnsdk;
    j["module"] = f.module_name;
    j["version"] = ver_json(f.version);
    j["module_id"] = f.module_id;
    j["sha256"] = f.sha256;
    j["size"] = f.size;
    return j;
}
SdkFile sdkfile_of(const json& j) {
    SdkFile f;
    f.nnsdk = flag(j, "nnsdk");
    f.module_name = str(j, "module");
    f.version = ver_of(j.at("version"));
    f.module_id = str(j, "module_id");
    f.sha256 = str(j, "sha256");
    if (!is_hex(f.sha256, 64) || !is_hex(f.module_id, 64)) throw std::runtime_error("bad sdk ids");
    if (!j.at("size").is_number_unsigned()) throw std::runtime_error("bad size");
    f.size = j.at("size").get<uint64_t>();
    return f;
}

// Общая шапка: объект, своя схема и свой app. Иначе — промах.
bool header_ok(const json& j, const std::string& app) {
    return j.is_object() && j.contains("schema") && j["schema"].is_number_integer() && j["schema"].get<int>() == kSchema &&
           j.contains("app") && j["app"].is_string() && j["app"].get<std::string>() == app;
}
json header(const std::string& app, const char* note) {
    json j = json::object();
    j["schema"] = kSchema;
    j["app"] = app;
    j["note"] = note;
    return j;
}
Bytes to_bytes(const json& j) {
    // Имена игр приходят из NACP: неверный UTF-8 не должен ронять запись кэша — заменяется.
    const std::string s = j.dump(1, ' ', false, json::error_handler_t::replace) + "\n";
    return Bytes(s.begin(), s.end());
}
json parse_json(const Bytes& data) { return json::parse(data.begin(), data.end()); }

}  // namespace

// ------------------------------------------------------------ ключи

std::string forecast_fingerprint() {
    std::string s = "rev=" + std::to_string(kForecastRev);
    for (const auto& f : autofix_catalog()) {
        for (const std::string* p : {&f.id, &f.symbol, &f.kind, &f.scope, &f.history, &f.own}) {
            s += '\x1f';
            s += *p;
        }
        s += '\x1e';
    }
    return sha256_hex(soft_hash(), Bytes(s.begin(), s.end())).substr(0, 16);
}

std::string app_key(const std::string& app_version) { return app_version + "/" + forecast_fingerprint(); }

FileStamp stamp_file(Env& env, const std::string& path, const MtimeFn& mtime) {
    FileStamp s;
    s.size = env.fs.file_size(path);
    s.head = sha256_hex(env.hash, env.fs.read_range(path, 0, kHeadSize));
    if (mtime) s.mtime = mtime(path);
    return s;
}

bool same_stamp(const FileStamp& a, const FileStamp& b) {
    if (a.size != b.size || a.head != b.head || a.head.empty()) return false;
    // mtime есть только с одной стороны — промах: размер и заголовок NSO (0x100) не отличат пропатченное тело.
    if (a.mtime.has_value() != b.mtime.has_value()) return false;
    if (a.mtime && *a.mtime != *b.mtime) return false;
    return true;
}

// ------------------------------------------------------------ sdk-files.json

SdkFilesCache parse_sdk_files(const Bytes& data, const std::string& app) {
    SdkFilesCache c;
    try {
        const json j = parse_json(data);
        if (!header_ok(j, app) || !j.contains("files") || !j["files"].is_object()) return c;
        for (const auto& [path, e] : j["files"].items()) {
            try {
                SdkFilesCache::Item it;
                it.stamp = stamp_of(e.at("stamp"));
                it.file = sdkfile_of(e.at("sdk"));
                c.files[path] = std::move(it);
            } catch (const std::exception&) {
                // запись без нужных полей — промах (проверим файл заново), а не «пустое значение»
            }
        }
    } catch (const std::exception&) {
        return SdkFilesCache{};
    }
    return c;
}

Bytes dump_sdk_files(const SdkFilesCache& c, const std::string& app) {
    json j = header(app, "ModKitNX cache: sdk files by fingerprint (size, mtime, sha256 of the first 256 bytes)");
    json files = json::object();
    for (const auto& [path, it] : c.files) {
        json e = json::object();
        e["stamp"] = stamp_json(it.stamp);
        e["sdk"] = sdkfile_json(it.file);
        files[path] = std::move(e);
    }
    j["files"] = std::move(files);
    return to_bytes(j);
}

std::vector<SdkEntry> scan_sdk_cached(Env& env, const std::vector<SdkRoot>& roots, SdkFilesCache& cache,
                                      const MtimeFn& mtime, ScanStats* stats, int max_depth,
                                      std::vector<SdkFile>* rejected) {
    ScanStats st;
    SdkFilesCache next;
    auto identify = [&](const std::string& path, const std::string& label) -> std::optional<SdkFile> {
        FileStamp stamp;
        try {
            stamp = stamp_file(env, path, mtime);
        } catch (const std::exception&) {
            st.misses++;
            return identify_sdk(env, path, label);   // отпечатка нет — живьём, в кэш не кладём
        }
        auto it = cache.files.find(path);
        if (it != cache.files.end() && same_stamp(it->second.stamp, stamp)) {
            st.hits++;
            SdkFile f = it->second.file;
            f.path = path;
            f.label = label;
            next.files[path] = it->second;
            return f;
        }
        st.misses++;
        auto f = identify_sdk(env, path, label);
        if (f) {
            next.files[path] = SdkFilesCache::Item{stamp, *f};
            st.changed = true;
        }
        return f;
    };
    std::vector<SdkEntry> out = scan_sdk_with(env, roots, identify, max_depth, rejected);
    if (next.files.size() != cache.files.size()) st.changed = true;   // исчезнувшие пути
    cache = std::move(next);
    if (stats) *stats = st;
    return out;
}

// ------------------------------------------------------------ games.json

GamesCache parse_games(const Bytes& data, const std::string& app) {
    GamesCache c;
    try {
        const json j = parse_json(data);
        if (!header_ok(j, app) || !j.contains("games") || !j["games"].is_object()) return c;
        for (const auto& [tid, e] : j["games"].items()) {
            try {
                if (!is_hex(tid, 16)) continue;
                GameEntry g;
                g.key = str(e, "key");
                if (g.key.empty()) continue;
                g.name = str(e, "name");
                g.version = str(e, "version");
                g.game_sdk = str(e, "game_sdk");
                g.icon = flag(e, "icon");
                const json& s = e.at("swap");
                if (!s.is_null()) {
                    SwapInfo w;
                    w.stamp = stamp_of(s.at("stamp"));
                    w.nnsdk = flag(s, "nnsdk");
                    w.version = str(s, "version");
                    w.sha256 = str(s, "sha256");
                    if (!is_hex(w.sha256, 64)) throw std::runtime_error("bad sha");
                    g.swap = w;
                }
                c.games[tid] = std::move(g);
            } catch (const std::exception&) {
                // запись игры с неверными полями — промах
            }
        }
    } catch (const std::exception&) {
        return GamesCache{};
    }
    return c;
}

Bytes dump_games(const GamesCache& c, const std::string& app) {
    json j = header(app, "ModKitNX cache: game list (name, version, sdk by NCA); icons in icons/<TID>.jpg");
    json games = json::object();
    for (const auto& [tid, g] : c.games) {
        json e = json::object();
        e["key"] = g.key;
        e["name"] = g.name;
        e["version"] = g.version;
        e["game_sdk"] = g.game_sdk;
        e["icon"] = g.icon;
        if (g.swap) {
            json s = json::object();
            s["stamp"] = stamp_json(g.swap->stamp);
            s["nnsdk"] = g.swap->nnsdk;
            s["version"] = g.swap->version;
            s["sha256"] = g.swap->sha256;
            e["swap"] = std::move(s);
        } else {
            e["swap"] = nullptr;
        }
        games[tid] = std::move(e);
    }
    j["games"] = std::move(games);
    return to_bytes(j);
}

// ------------------------------------------------------------ game/<TID>.json

ForecastGame compact_game(const ForecastGame& g) {
    ForecastGame c = g;
    c.all_imports.clear();
    c.module_exports.clear();
    c.own_sdk.clear();
    if (!g.own_sdk_ok) return c;   // пробелы не считаются вовсе (fixesView смотрит own_sdk_ok)
    for (const auto& [s, gi] : g.all_imports) {
        if (g.module_exports.count(s)) continue;
        auto o = g.own_sdk.find(s);
        if (o == g.own_sdk.end()) continue;
        c.all_imports[s] = gi;
        c.own_sdk[s] = o->second;
    }
    return c;
}

ForecastSdk restrict_sdk(const ForecastSdk& s, const ForecastGame& compacted) {
    ForecastSdk r = s;
    r.exports.clear();
    std::set<std::string> cat;
    for (const auto& f : autofix_catalog()) cat.insert(f.symbol);
    for (const auto& [sym, stub] : s.exports)
        if (compacted.all_imports.count(sym) || cat.count(sym)) r.exports[sym] = stub;
    return r;
}

json game_to_json(const ForecastGame& g) {
    json j = json::object();
    j["version"] = ver_json(g.version);
    j["vulkan"] = g.vulkan;
    j["vulkan_module"] = g.vulkan_module;
    j["opengl"] = g.opengl;
    json imp = json::object();
    for (const auto& [s, mods] : g.imports) imp[s] = mods;
    j["imports"] = std::move(imp);
    json all = json::object();
    for (const auto& [s, gi] : g.all_imports) all[s] = json{{"weak", gi.weak}, {"by", gi.importers}};
    j["all_imports"] = std::move(all);
    j["module_exports"] = g.module_exports;
    j["modules"] = g.modules;
    j["gnu_hash_modules"] = g.gnu_hash_modules;
    j["own_sdk_ok"] = g.own_sdk_ok;
    j["own_sdk_error"] = g.own_sdk_error;
    json own = json::object();
    for (const auto& [s, stub] : g.own_sdk) own[s] = stub;
    j["own_sdk"] = std::move(own);
    return j;
}

bool game_from_json(const json& j, ForecastGame& out) {
    try {
        ForecastGame g;
        g.version = ver_of(j.at("version"));
        g.vulkan = flag(j, "vulkan");
        g.vulkan_module = str(j, "vulkan_module");
        g.opengl = flag(j, "opengl");
        for (const auto& [s, mods] : j.at("imports").items()) g.imports[s] = strs(mods);
        for (const auto& [s, gi] : j.at("all_imports").items()) {
            GameImport x;
            x.weak = flag(gi, "weak");
            x.importers = strs(gi.at("by"));
            g.all_imports[s] = std::move(x);
        }
        for (const auto& s : strs(j.at("module_exports"))) g.module_exports.insert(s);
        g.modules = strs(j.at("modules"));
        g.gnu_hash_modules = strs(j.at("gnu_hash_modules"));
        g.own_sdk_ok = flag(j, "own_sdk_ok");
        g.own_sdk_error = str(j, "own_sdk_error");
        for (const auto& [s, stub] : j.at("own_sdk").items()) g.own_sdk[s] = stub.get<bool>();
        out = std::move(g);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

json sdk_to_json(const ForecastSdk& s) {
    json j = json::object();
    j["name"] = s.name ? json(*s.name) : json(nullptr);
    j["known"] = s.known;
    j["version"] = ver_json(s.version);
    json ex = json::object();
    for (const auto& [sym, stub] : s.exports) ex[sym] = stub;
    j["exports"] = std::move(ex);
    j["ro_lookup"] = s.ro_lookup;
    return j;
}

bool sdk_from_json(const json& j, ForecastSdk& out) {
    try {
        ForecastSdk s;
        if (!j.at("name").is_null()) s.name = j.at("name").get<std::string>();
        s.known = flag(j, "known");
        s.version = ver_of(j.at("version"));
        for (const auto& [sym, stub] : j.at("exports").items()) s.exports[sym] = stub.get<bool>();
        s.ro_lookup = str(j, "ro_lookup");
        out = std::move(s);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<GameCheck> parse_game_check(const Bytes& data, const std::string& app, const std::string& key,
                                          const std::string& fw, const std::string& ams) {
    if (key.empty()) return std::nullopt;
    try {
        const json j = parse_json(data);
        if (!header_ok(j, app)) return std::nullopt;
        GameCheck c;
        c.key = str(j, "key");
        c.fw = str(j, "fw");
        c.ams = str(j, "ams");
        if (c.key != key || c.fw != fw || c.ams != ams) return std::nullopt;
        c.exefs = j.at("exefs");
        if (!c.exefs.is_object()) return std::nullopt;
        c.zbic = flag(j, "zbic");
        c.zbic_files = strs(j.at("zbic_files"));
        for (const auto& [n, id] : j.at("module_ids").items()) c.module_ids[n] = id.get<std::string>();
        if (!game_from_json(j.at("game"), c.game)) return std::nullopt;
        const json& own = j.at("own_side");
        if (!own.is_null()) {
            ForecastSdk s;
            if (!sdk_from_json(own, s)) return std::nullopt;
            c.own_side = std::move(s);
        }
        for (const auto& [sha, e] : j.at("by_sdk").items()) {
            ForecastSdk s;
            if (is_hex(sha, 64) && sdk_from_json(e, s)) c.by_sdk[sha] = std::move(s);
        }
        return c;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Bytes dump_game_check(const GameCheck& c, const std::string& app) {
    json j = header(app, "ModKitNX cache: game check (display only; install and removal always read the game itself)");
    j["key"] = c.key;
    j["fw"] = c.fw;
    j["ams"] = c.ams;
    j["exefs"] = c.exefs;
    j["zbic"] = c.zbic;
    j["zbic_files"] = c.zbic_files;
    json ids = json::object();
    for (const auto& [n, id] : c.module_ids) ids[n] = id;
    j["module_ids"] = std::move(ids);
    j["game"] = game_to_json(c.game);
    j["own_side"] = c.own_side ? sdk_to_json(*c.own_side) : json(nullptr);
    json by = json::object();
    for (const auto& [sha, s] : c.by_sdk) by[sha] = sdk_to_json(s);
    j["by_sdk"] = std::move(by);
    return to_bytes(j);
}

// ------------------------------------------------------------ файлы

Bytes read_cache_file(Env& env, const std::string& path) {
    try {
        if (!env.fs.is_file(path) || env.fs.file_size(path) > kMaxFileSize) return {};
        return env.fs.read_file(path);
    } catch (const std::exception&) {
        return {};
    }
}

void write_cache_file(Env& env, const std::string& path, const Bytes& data) {
    env.fs.make_dirs(path_parent(path));
    env.fs.write_file_replace(path, data.data(), data.size());
}

size_t remove_cache_files(Env& env, const std::string& cache_dir, const std::set<std::string>* keep, bool top_files) {
    size_t n = 0;
    auto drop = [&](const std::string& p) {
        try {
            if (env.fs.is_file(p)) {
                env.fs.remove_file(p);
                ++n;
            }
        } catch (const std::exception&) {
        }
    };
    auto sweep = [&](const std::string& sub, const std::string& ext) {
        const std::string dir = path_join(cache_dir, sub);
        if (!env.fs.is_dir(dir)) return;
        std::vector<DirEntry> list;
        try {
            list = env.fs.list_dir(dir);
        } catch (const std::exception&) {
            return;
        }
        for (const auto& e : list) {
            if (e.is_dir || e.name.size() != 16 + ext.size() || e.name.compare(16, ext.size(), ext) != 0) continue;
            std::string tid = e.name.substr(0, 16);
            if (!is_hex(tid, 16)) continue;
            for (auto& ch : tid) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (keep && keep->count(tid)) continue;
            drop(path_join(dir, e.name));
        }
    };
    sweep("icons", ".jpg");
    sweep("game", ".json");
    if (top_files)
        for (const char* f : {"games.json", "sdk-files.json", "sdk-scan.json"}) drop(path_join(cache_dir, f));
    return n;
}

}  // namespace mk::cache
