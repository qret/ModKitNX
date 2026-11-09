// sdk из установленных игр: запись игры, кэш, выбор кандидатов, имя папки, сохранение, подпись.
// См. mk/sdk_harvest.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/sdk_harvest.hpp"
#include "mk/nacp.hpp"
#include "mk/nso.hpp"

#include <algorithm>

namespace mk::harvest {

using oj = nlohmann::ordered_json;

namespace {

constexpr const char* kKind = "from_installed_game";
constexpr size_t kGameNameMax = 40;   // символов названия игры в имени папки

std::optional<Ver3> parse_ver(const std::string& s) {
    Ver3 v{};
    int part = 0;
    size_t i = 0;
    if (s.empty()) return std::nullopt;
    while (i < s.size() && part < 3) {
        size_t st = i;
        long n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - st < 6) n = n * 10 + (s[i++] - '0');
        if (i == st) return std::nullopt;
        v[part++] = int(n);
        if (i < s.size()) {
            if (s[i] != '.') return std::nullopt;
            ++i;
        }
    }
    if (part != 3 || i != s.size()) return std::nullopt;
    return v;
}

std::string str_of(const oj& j, const char* k) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

oj game_json(const GameSdk& g) {
    oj j = oj::object();
    j["title_id"] = g.tid;
    j["name"] = g.name;
    j["version"] = g.version;
    return j;
}

// Обрезать UTF-8 до n символов (не байтов) — середину символа не резать.
std::string utf8_prefix(const std::string& s, size_t n) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (chars == n) return s.substr(0, i);
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        i += len;
        ++chars;
    }
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ------------------------------------------------------------ одна игра
GameSdk probe_sdk_file(Env& env, const std::string& sdk_path, GameSdk g) {
    g.has_sdk = g.nnsdk = g.zbic = false;
    g.module_name.clear();
    g.sdk_version.reset();
    g.module_id.clear();
    g.sha256.clear();
    g.size = 0;
    g.checked = env.clock.iso_seconds();
    if (!env.fs.is_file(sdk_path)) return g;
    auto f = identify_sdk(env, sdk_path, "game");
    // Файл sdk в exefs есть, но не прочитался как NSO: возможна временная ошибка чтения — не записывать игру
    // в кэш как «sdk нет» (запись живёт до обновления игры), а считать сбоем и проверить в следующий раз.
    if (!f) throw Error("SDK_UNREADABLE", "sdk игры не прочитан как NSO: " + sdk_path);
    g.has_sdk = true;
    g.nnsdk = f->nnsdk;
    g.module_name = f->module_name;
    g.sdk_version = f->version;
    g.module_id = f->module_id;
    g.sha256 = f->sha256;
    g.size = f->size;
    Bytes h = env.fs.read_range(sdk_path, 0, kNsoHeaderSize);
    NsoHeader hdr;
    g.zbic = parse_nso_header(h.data(), h.size(), hdr) && hdr.zbic();
    return g;
}

// ------------------------------------------------------------ кэш
// Разбор без защиты: неверный тип поля («"schema":"1"», null вместо числа) бросает nlohmann type_error.
static Cache parse_cache_body(const Bytes& data) {
    Cache c;
    oj j = oj::parse(std::string(data.begin(), data.end()));
    if (!j.is_object() || j.value("schema", 0) != kCacheSchema || !j.contains("games") || !j["games"].is_array())
        return c;
    for (const auto& e : j["games"]) {
        if (!e.is_object()) continue;
        GameSdk g;
        g.tid = str_of(e, "title_id");
        g.key = str_of(e, "key");
        if (g.tid.size() != 16 || g.key.empty()) continue;
        g.name = str_of(e, "name");
        g.version = str_of(e, "version");
        g.checked = str_of(e, "checked");
        auto s = e.find("sdk");
        if (s != e.end() && s->is_object()) {
            g.has_sdk = true;
            g.nnsdk = s->value("nnsdk", false);
            g.module_name = str_of(*s, "module");
            g.sdk_version = parse_ver(str_of(*s, "version"));
            g.module_id = str_of(*s, "module_id");
            g.sha256 = str_of(*s, "sha256");
            g.size = s->value("size", uint64_t(0));
            g.zbic = s->value("zbic", false);
            if (g.sha256.size() != 64) continue;   // запись без sha256 — не доверяем, проверим заново
        }
        cache_put(c, g);
    }
    return c;
}

// Любой сбой разбора (битый JSON, чужие типы полей) — пустой кэш: проверим всё заново, а не std::terminate.
Cache parse_cache(const Bytes& data) {
    try {
        return parse_cache_body(data);
    } catch (const std::exception&) {
        return Cache{};
    }
}

Bytes dump_cache(const Cache& c) {
    oj j = oj::object();
    j["schema"] = kCacheSchema;
    j["note"] = "sdk of installed games (ModKitNX): rescanned only when a game is updated or reinstalled";
    oj arr = oj::array();
    for (const auto& g : c.games) {
        oj e = oj::object();
        e["title_id"] = g.tid;
        e["name"] = g.name;
        e["version"] = g.version;
        e["key"] = g.key;
        if (g.has_sdk) {
            oj s = oj::object();
            s["nnsdk"] = g.nnsdk;
            s["module"] = g.module_name;
            s["version"] = g.sdk_version ? oj(version_text(*g.sdk_version)) : oj("");
            s["module_id"] = g.module_id;
            s["sha256"] = g.sha256;
            s["size"] = g.size;
            s["zbic"] = g.zbic;
            e["sdk"] = s;
        } else {
            e["sdk"] = nullptr;
        }
        e["checked"] = g.checked;
        arr.push_back(e);
    }
    j["games"] = arr;
    std::string t = j.dump(2, ' ', false, nlohmann::detail::error_handler_t::replace) + "\n";
    return Bytes(t.begin(), t.end());
}

Cache load_cache(Env& env, const std::string& path) {
    if (!env.fs.is_file(path)) return Cache{};
    return parse_cache(env.fs.read_file(path));
}

void save_cache(Env& env, const std::string& path, const Cache& c) {
    env.fs.make_dirs(path_parent(path));
    Bytes b = dump_cache(c);
    env.fs.write_file_replace(path, b.data(), b.size());
}

const GameSdk* cache_hit(const Cache& c, const std::string& tid, const std::string& key) {
    for (const auto& g : c.games)
        if (g.tid == tid && g.key == key && !key.empty()) return &g;
    return nullptr;
}

void cache_put(Cache& c, const GameSdk& g) {
    for (auto& e : c.games)
        if (e.tid == g.tid) {
            e = g;
            return;
        }
    c.games.push_back(g);
}

// ------------------------------------------------------------ выбор
Selection select_candidates(const std::vector<GameSdk>& games, const std::optional<Ver3>& firmware,
                            const std::vector<std::pair<std::string, std::string>>& present) {
    Selection s;
    s.firmware = firmware;
    s.games = int(games.size());
    if (firmware) {
        const FwSdkRow* row = nullptr;
        for (const auto& r : fw_sdk_table())
            if (r.hos_major <= (*firmware)[0] && (!row || r.hos_major > row->hos_major)) row = &r;
        if (row) {
            s.table_sdk = row->sdk;
            s.table_source = row->source;
        }
    }
    auto is_present = [&](const GameSdk& g) {
        for (const auto& [sha, mid] : present)
            if ((!sha.empty() && sha == g.sha256) || (!mid.empty() && mid == g.module_id)) return true;
        return false;
    };
    // Игры по названию — чтобы источник (первая игра строки) был предсказуемым.
    std::vector<const GameSdk*> order;
    for (const auto& g : games) order.push_back(&g);
    std::stable_sort(order.begin(), order.end(), [](const GameSdk* a, const GameSdk* b) {
        const std::string x = lower(a->name), y = lower(b->name);
        return x != y ? x < y : a->tid < b->tid;
    });
    for (const GameSdk* gp : order) {
        const GameSdk& g = *gp;
        if (!g.has_sdk) continue;
        if (!g.nnsdk || !g.sdk_version) {
            s.not_nnsdk++;
            continue;
        }
        s.with_nnsdk++;
        if (!firmware) continue;
        if ((*g.sdk_version)[0] > (*firmware)[0]) {
            s.too_new++;
            continue;
        }
        if (is_present(g)) {
            s.already++;
            continue;
        }
        auto it = std::find_if(s.candidates.begin(), s.candidates.end(), [&](const Candidate& c) {
            return c.sha256 == g.sha256 || (!g.module_id.empty() && c.module_id == g.module_id);
        });
        if (it != s.candidates.end()) {
            it->games.push_back(g);
            continue;
        }
        Candidate c;
        c.version = *g.sdk_version;
        c.sha256 = g.sha256;
        c.module_id = g.module_id;
        c.size = g.size;
        c.zbic = g.zbic;
        c.recommended = s.table_sdk && *s.table_sdk == c.version;
        c.games.push_back(g);
        s.candidates.push_back(c);
    }
    // Нижняя граница: есть sdk с мажором прошивки — более старые мажоры не показывать (выбор без лишнего);
    // нет ни одной — показать всё, что не новее прошивки (последний шанс).
    if (firmware) {
        const int fw_major = (*firmware)[0];
        const bool has_same = std::any_of(s.candidates.begin(), s.candidates.end(),
                                          [&](const Candidate& c) { return c.version[0] == fw_major; });
        if (has_same) {
            for (const auto& c : s.candidates)
                if (c.version[0] < fw_major) s.older_hidden += int(c.games.size());
            s.candidates.erase(std::remove_if(s.candidates.begin(), s.candidates.end(),
                                              [&](const Candidate& c) { return c.version[0] < fw_major; }),
                               s.candidates.end());
        }
    }
    for (auto& a : s.candidates)
        for (auto& b : s.candidates)
            if (&a != &b && a.version == b.version) a.other_build = true;
    std::stable_sort(s.candidates.begin(), s.candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.recommended != b.recommended) return a.recommended;
        if (a.version != b.version) return a.version > b.version;   // ближе к прошивке — выше
        return a.games.size() > b.games.size();
    });
    return s;
}

// ------------------------------------------------------------ имя и сохранение
std::string folder_name(const Candidate& c) {
    std::string game = c.games.empty() ? std::string() : c.games.front().name;
    game = safe_folder_name(utf8_prefix(game, kGameNameMax), "game");
    return safe_folder_name("nnSdk " + version_text(c.version) + " (" + game + ")", "nnSdk " + version_text(c.version));
}

SaveTarget save_target(Env& env, const std::string& root, const Candidate& c) {
    const std::string base = folder_name(c);
    for (int n = 1; n <= 20; ++n) {
        SaveTarget t;
        t.dir = path_join(root, n == 1 ? base : base + " " + std::to_string(n));
        t.sdk_path = path_join(t.dir, "sdk");
        t.source_path = path_join(t.dir, "source.json");
        if (!env.fs.exists(t.dir)) return t;
        if (env.fs.is_dir(t.dir) && env.fs.is_file(t.sdk_path) && file_sha256(env, t.sdk_path) == c.sha256) {
            t.already = true;
            return t;
        }
        if (env.fs.is_dir(t.dir) && !env.fs.exists(t.sdk_path) && !env.fs.exists(t.source_path)) return t;
    }
    return SaveTarget{};   // 20 занятых папок — не пишем
}

oj source_json(const Candidate& c, const std::string& saved_iso, const std::string& app) {
    oj j = oj::object();
    j["schema"] = 1;
    j["kind"] = kKind;
    j["note"] = "взято из установленной игры (taken from an installed game); nothing was downloaded";
    j["sdk_version"] = version_text(c.version);
    j["module"] = "nnSdk";
    j["module_id"] = c.module_id;
    j["sha256"] = c.sha256;
    j["size"] = c.size;
    j["zbic"] = c.zbic;
    j["game"] = c.games.empty() ? oj(nullptr) : game_json(c.games.front());
    oj also = oj::array();
    for (size_t i = 1; i < c.games.size(); ++i) also.push_back(game_json(c.games[i]));
    j["also_in_games"] = also;
    j["saved"] = saved_iso;
    j["saved_by"] = app;
    return j;
}

SaveResult save_candidate(Env& env, const std::string& root, const Candidate& c, const std::string& src_path,
                          const std::string& app) {
    SaveResult r;
    try {
        r.target = save_target(env, root, c);
        if (r.target.dir.empty()) {
            r.code = "no_room";
            return r;
        }
        Bytes data = env.fs.read_file(src_path);
        if (sha256_hex(env.hash, data) != c.sha256) {
            r.code = "changed";
            r.detail = "sha256 файла в игре не совпал с найденным: " + src_path;
            return r;
        }
        NsoImage n(data, &env.hash);
        if (n.module_name().value_or("") != "nnSdk" || n.mw_version() != std::optional<Ver3>(c.version)) {
            r.code = "not_nnsdk";
            return r;
        }
        if (!r.target.already) {
            env.fs.make_dirs(r.target.dir);
            if (!env.fs.exists(r.target.sdk_path)) env.fs.write_file(r.target.sdk_path, data.data(), data.size());
            if (file_sha256(env, r.target.sdk_path) != c.sha256) {
                r.code = "changed";
                r.detail = "записанный файл не совпал по sha256: " + r.target.sdk_path;
                return r;
            }
        }
        if (!env.fs.exists(r.target.source_path)) {
            std::string t = source_json(c, env.clock.iso_seconds(), app).dump(2, ' ', false,
                                                                             nlohmann::detail::error_handler_t::replace) +
                            "\n";
            env.fs.write_file(r.target.source_path, reinterpret_cast<const uint8_t*>(t.data()), t.size());
        }
        r.ok = true;
    } catch (const Error& e) {
        r.code = e.code();
        r.detail = e.what();
    } catch (const std::exception& e) {
        r.code = "FAIL_IO";
        r.detail = e.what();
    }
    return r;
}

std::optional<SourceInfo> read_source(Env& env, const std::string& sdk_path, const std::string& sha256) {
    const std::string p = path_join(path_parent(sdk_path), "source.json");
    try {
        if (!env.fs.is_file(p) || env.fs.file_size(p) > 64 * 1024) return std::nullopt;
        Bytes b = env.fs.read_file(p);
        oj j = oj::parse(std::string(b.begin(), b.end()));
        if (!j.is_object() || str_of(j, "kind") != kKind || str_of(j, "sha256") != sha256 || sha256.empty())
            return std::nullopt;
        SourceInfo s;
        s.sdk_version = str_of(j, "sdk_version");
        auto g = j.find("game");
        if (g != j.end() && g->is_object()) {
            s.game = str_of(*g, "name");
            s.game_version = str_of(*g, "version");
            s.title_id = str_of(*g, "title_id");
        }
        if (s.game.empty()) return std::nullopt;
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace mk::harvest
