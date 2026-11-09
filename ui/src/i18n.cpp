// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — перевод интерфейса и выбор языка. См. ui/i18n.hpp.
#include "ui/i18n.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "platform/app_config.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"

namespace mk::ui::i18n {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string i18nDir() { return std::string(BRLS_RESOURCES) + "i18n/"; }

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Все *.json папки языка: имя файла без .json — первый уровень ключа (как у borealis).
json loadDir(const std::string& code) {
    json out = json::object();
    std::error_code ec;
    const std::string dir = i18nDir() + code;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        std::string text;
        if (!readFile(it->path().string(), text)) continue;
        try {
            out[name.substr(0, name.size() - 5)] = json::parse(text);
        } catch (const std::exception& e) {
            MK_LOG("i18n: %s/%s: %s", code.c_str(), name.c_str(), e.what());
        }
    }
    if (ec) MK_LOG("i18n: no folder %s", dir.c_str());
    return out;
}

std::mutex g_mtx;
json g_cur = json::object(), g_def = json::object();
std::string g_lang;
bool g_defLoaded = false;

bool lookup(const json& j, const std::string& key, std::string& out) {
    const json* p = &j;
    size_t pos = 0;
    while (pos <= key.size()) {
        const size_t slash = key.find('/', pos);
        const std::string part = key.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!p->is_object()) return false;
        auto it = p->find(part);
        if (it == p->end()) return false;
        p = &*it;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    if (!p->is_string()) return false;
    out = p->get<std::string>();
    return true;
}

}  // namespace

const std::vector<Language>& languages() {
    static std::vector<Language> list = [] {
        std::vector<Language> v;
        std::string text;
        if (readFile(i18nDir() + "languages.json", text)) {
            try {
                const json j = json::parse(text);
                for (const auto& l : j.value("languages", json::array())) {
                    Language x{l.value("code", ""), l.value("name", ""), l.value("english", ""), l.value("choose", "")};
                    if (x.name.empty()) x.name = x.code;
                    if (!x.code.empty()) v.push_back(x);
                }
            } catch (const std::exception& e) {
                MK_LOG("i18n: languages.json: %s", e.what());
            }
        }
        if (v.empty()) v.push_back({kFallback, "English", "English", "Choose a language"});
        return v;
    }();
    return list;
}

bool known(const std::string& code) {
    for (const auto& l : languages())
        if (l.code == code) return true;
    return false;
}

std::string bestMatch(const std::string& locale) {
    if (known(locale)) return locale;
    auto base = [](const std::string& c) {
        std::string b = c.substr(0, c.find_first_of("-_"));
        for (auto& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return b;
    };
    const std::string want = base(locale);
    if (!want.empty())
        for (const auto& l : languages())
            if (base(l.code) == want) return l.code;
    return kFallback;
}

void setLanguage(const std::string& code) {
    const std::string c = known(code) ? code : kFallback;
    json def = g_defLoaded ? json() : loadDir(kFallback);
    json cur = (c == kFallback) ? json::object() : loadDir(c);
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_defLoaded) {
        g_def = std::move(def);
        g_defLoaded = true;
    }
    g_cur = std::move(cur);
    g_lang = c;
    MK_LOG("i18n: language %s", c.c_str());
}

const std::string& language() { return g_lang; }

std::string raw(const std::string& key, bool* found) {
    std::string out;
    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        ok = lookup(g_cur, key, out) || lookup(g_def, key, out);
    }
    if (found) *found = ok;
    return ok ? out : std::string();
}

std::string rawIn(const std::string& code, const std::string& key) {
    static std::mutex mtx;
    static std::map<std::string, json> cache;
    const std::string c = known(code) ? code : kFallback;
    std::lock_guard<std::mutex> lock(mtx);
    auto get = [](const std::string& k) -> const json& {
        auto it = cache.find(k);
        if (it == cache.end()) it = cache.emplace(k, loadDir(k)).first;
        return it->second;
    };
    std::string out;
    if (lookup(get(c), key, out) || lookup(get(kFallback), key, out)) return out;
    return std::string();
}

std::string format(const std::string& tmpl, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(tmpl.size() + 32);
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '{') {
            size_t j = i + 1;
            while (j < tmpl.size() && tmpl[j] >= '0' && tmpl[j] <= '9') ++j;
            if (j > i + 1 && j < tmpl.size() && tmpl[j] == '}') {
                const size_t n = std::stoul(tmpl.substr(i + 1, j - i - 1));
                out += n < args.size() ? args[n] : std::string();
                i = j;
                continue;
            }
        }
        out += tmpl[i];
    }
    return out;
}

std::string savedLanguage() {
    const std::string c = mk::plat::config::getString(mk::plat::config::kLanguage);
    if (known(c)) return c;
    // Прежний settings.ini (до трёх языков): lang=ru|en — это уже сделанный выбор.
    std::string text;
    if (readFile(mk::plat::paths::kSettingsFile, text)) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line == "lang=ru" && known("ru")) return "ru";
            if (line == "lang=en") return kFallback;
        }
    }
    return "";
}

// Запись — общим кодом настроек (platform/app_config: .part -> переименование, прежний файл — .prev).
bool saveLanguage(const std::string& code) { return mk::plat::config::setString(mk::plat::config::kLanguage, code); }

}  // namespace mk::ui::i18n
