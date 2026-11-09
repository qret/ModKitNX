// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — журнал. Простой синхронный (fflush после строки): журнал нужен как раз когда что-то упало.
// Идея — TorrentShopNX (source/utils/log.*), код свой.
#include "platform/log.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

#include "platform/app_config.hpp"
#include "platform/paths.hpp"

namespace mk::plat::log {

namespace {
std::mutex g_mtx;
FILE* g_file = nullptr;
std::string g_path;
}  // namespace

// Свои журналы прошлых запусков: оставить kKeepFiles - 1 самых новых (имя содержит дату — сортировка по имени).
void pruneOld() {
    const std::string prefix = std::string(MK_APP_NAME) + "-";
    std::vector<std::string> names;
    if (DIR* d = ::opendir(paths::kLogsDir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > prefix.size() + 4 && n.compare(0, prefix.size(), prefix) == 0 &&
                n.compare(n.size() - 4, 4, ".log") == 0)
                names.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    const size_t keep = kKeepFiles > 1 ? static_cast<size_t>(kKeepFiles - 1) : 0;
    for (size_t i = 0; i + keep < names.size(); ++i) ::unlink((paths::kLogsDir + names[i]).c_str());
}

void open() {
    if (!mk::plat::config::getBool(mk::plat::config::kAppLog, false)) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_file) return;
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir(paths::kAppDir.c_str(), 0777);
    ::mkdir(paths::kLogsDir.c_str(), 0777);
    pruneOld();
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char name[64];
    std::strftime(name, sizeof(name), "-%Y%m%d-%H%M%S.log", &tmv);
    g_path = paths::kLogsDir + MK_APP_NAME + name;
    g_file = std::fopen(g_path.c_str(), "ab");
    if (!g_file) g_path.clear();
}

void close() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

bool enabled() { return mk::plat::config::getBool(mk::plat::config::kAppLog, false); }

bool setEnabled(bool on) {
    if (!mk::plat::config::setBool(mk::plat::config::kAppLog, on)) return false;
    if (on) {
        open();
        line("log: enabled from settings");
    } else {
        line("log: disabled from settings");
        close();
    }
    return true;
}

std::string currentPath() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_path;
}

void line(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_file) return;
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S ", &tmv);
    std::fputs(ts, g_file);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_file, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

}  // namespace mk::plat::log
