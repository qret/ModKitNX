// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — настройки приложения (config.json). См. platform/app_config.hpp.
#include "platform/app_config.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "platform/log.hpp"
#include "platform/paths.hpp"

namespace mk::plat::config {

namespace {

using json = nlohmann::json;

std::mutex g_mtx;
std::string g_path;   // пусто — paths::kConfigFile

const std::string& path() { return g_path.empty() ? mk::plat::paths::kConfigFile : g_path; }

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

json readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return json::object();
    std::ostringstream ss;
    ss << f.rdbuf();
    try {
        json j = json::parse(ss.str());
        return j.is_object() ? j : json::object();
    } catch (const std::exception& e) {
        MK_LOG("config: %s: %s", p.c_str(), e.what());
        return json::object();
    }
}

// config.json нет, а config.json.prev есть — запись оборвалась между «p → .prev» и «.part → p»: прежние
// настройки — в .prev; без этого следующая запись поля потеряла бы все остальные поля.
json readLocked() {
    const std::string& p = path();
    if (!exists(p) && exists(p + ".prev")) return readFile(p + ".prev");
    return readFile(p);
}

bool writeField(const std::string& key, const json& value) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const std::string& p = path();
    if (g_path.empty()) ::mkdir(mk::plat::paths::kAppDir.c_str(), 0777);
    json j = readLocked();
    j[key] = value;
    const std::string text = j.dump(2) + "\n";
    const std::string part = p + ".part", prev = p + ".prev";
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) {
        MK_LOG("config: cannot write %s", part.c_str());
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        MK_LOG("config: short write %s", part.c_str());
        return false;
    }
    // Свой файл настроек: одна прежняя копия .prev. rename на карте Switch не перезаписывает — старую .prev убрать.
    if (exists(p)) {
        if (exists(prev)) std::remove(prev.c_str());
        std::rename(p.c_str(), prev.c_str());
    }
    const bool done = std::rename(part.c_str(), p.c_str()) == 0;
    MK_LOG("config: %s -> %s", key.c_str(), done ? "saved" : "FAILED");
    return done;
}

}  // namespace

std::string getString(const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const json j = readLocked();
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}

bool getBool(const std::string& key, bool def) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const json j = readLocked();
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

bool setString(const std::string& key, const std::string& value) { return writeField(key, value); }
bool setBool(const std::string& key, bool value) { return writeField(key, value); }

void setPathForTests(const std::string& p) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_path = p;
}

}  // namespace mk::plat::config
