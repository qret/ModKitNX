// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Найти sdk в других играх»: данные для экрана (без ядра и libnx). Строки перевода экрана —
// resources/i18n/<язык>/sdkscan.json (ключи «sdkscan/…»).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app_types.hpp"

namespace mk::app {

struct HarvestGame {
    std::string tid, name, version;
};

// Одна строка выбора: файл sdk (дубли по sha256/ModuleId — одной строкой), игры, где он лежит.
struct HarvestLine {
    std::string version;            // «19.3.13»
    bool recommended = false;       // версия из таблицы ядра для этой прошивки
    bool otherBuild = false;        // та же версия, другой файл
    bool zbic = false;
    std::string sha256, moduleId;
    uint64_t size = 0;
    std::vector<HarvestGame> games; // первая — источник (по ней имя папки и подпись «из игры …»)
    std::string targetDir;          // куда сохранится: sdmc:/switch/<APP>/sdk/nnSdk 19.3.13 (Celeste)
    bool targetAlready = false;     // ровно этот файл там уже лежит
};

struct HarvestView {
    bool ok = false;                // поиск дошёл до выбора (может быть и пустым)
    bool cancelled = false;
    Text error;
    size_t total = 0, checked = 0, fromCache = 0, failed = 0;
    int withNnSdk = 0, tooNew = 0, already = 0, notNnSdk = 0;
    std::string firmware;           // «19.0.1»; пусто — прошивка не прочитана
    std::string tableSdk;           // что таблица ядра советует для прошивки («19.3.13»), пусто — нет строки
    std::vector<HarvestLine> lines; // порядок ядра: рекомендованная, затем ближайшие
};

struct HarvestSaveView {
    bool ok = false;
    bool already = false;           // такой файл уже лежал — ничего не записано
    Text error;
    std::string dir, sdkPath, version, game, gameVersion;
};

}  // namespace mk::app
