// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — разбор отчётов отладочной сборки модуля nxredirect (без libnx).
// Разбор отчёта модуля (parse_report, verdict, parse_gfx, gfx_verdict): этапы и порядок выводов заданы
// здесь. Формат строк отчёта задан модулем
// (`nxredirect/src/main.cpp`, Diag::Begin / Diag::LogOpen). Тексты выводов — не здесь, а ключами
// i18n (`app/diag/...`), чтобы были RU и EN.
//
// Отчёты пишет модуль на карту: sdmc:/nxredirect-diag.txt (подмена файлов) и sdmc:/nxredirect-gfx.txt (графика).
#pragma once

#include <string>
#include <vector>

namespace mk::plat::diag {

inline const char* kDiagPath = "sdmc:/nxredirect-diag.txt";
inline const char* kGfxPath = "sdmc:/nxredirect-gfx.txt";

enum class StageState { Ok, Fail, Unknown };

struct Stage {
    std::string code;    // loaded, tid, modpath, modules, symbols, mount, modfolder, redirect | hooks, surface, calls
    StageState state = StageState::Unknown;
    std::string detail;  // как в отчёте (строки модуля — русские, показываются как есть)
};

struct Verdict {
    std::string key;                // ключ i18n без префикса: «diag/v_...»
    std::vector<std::string> args;  // подстановки {0}, {1}
};

struct DiagSummary {
    bool isReport = false;
    std::string tid;      // как в отчёте
    std::string modPath;
    std::vector<Stage> stages;
    int opens = 0, attempts = 0, taken = 0;
    std::vector<std::pair<std::string, int>> reasons;  // причина отказа → сколько раз
    bool truncated = false;
    Verdict verdict;
};

struct GfxSummary {
    bool isReport = false;
    int hooksSet = -1, hooksTotal = -1;
    int calls = 0;
    std::vector<std::string> suspects;  // строки «!!» модуля: «вызов = результат»
    std::string firstEglError;
    std::vector<Stage> stages;
    Verdict verdict;
};

DiagSummary parseDiag(const std::string& text);
GfxSummary parseGfx(const std::string& text);

// Прочитать файл целиком (не больше maxBytes). false — файла нет.
bool readText(const char* path, std::string& out, size_t maxBytes = 512 * 1024);

}  // namespace mk::plat::diag
