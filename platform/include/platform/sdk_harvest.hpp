// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Найти sdk в других играх» на консоли: обход установленных игр, чтение ТОЛЬКО файла sdk каждой игры
// тем же путём, что для установки (exefs: Code FS по program NCA → Code FS с патчем; platform/exefs.hpp),
// кэш проверенных игр sdmc:/switch/<APP>/cache/sdk-scan.json (пересканируются только новые и обновлённые игры),
// сохранение выбранной sdk из exefs игры-источника. Ничего не скачивается и не отправляется.
// Опознание, кэш, выбор кандидатов, имя папки и source.json — ядро (core/include/mk/sdk_harvest.hpp).
// Всё блокирующее (IPC, карта) — из фонового потока.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <mk/sdk_harvest.hpp>

#include "platform/paths.hpp"

namespace mk::plat {

inline const std::string kSdkScanCache = paths::kAppDir + "cache/sdk-scan.json";

struct HarvestScan {
    size_t total = 0;        // установленных игр
    size_t checked = 0;      // проверено (вместе с взятыми из кэша)
    size_t fromCache = 0;    // из них — по кэшу (игра не менялась)
    size_t failed = 0;       // код игры не открылся (нет program NCA, Code FS отказала)
    bool cancelled = false;
    std::vector<mk::harvest::GameSdk> games;   // проверенные игры (для mk::harvest::select_candidates)
};

// progress(проверено, всего, название игры) — после каждой игры. cancel — остановиться после текущей игры
// (проверенное сохраняется в кэш).
using HarvestProgress = std::function<void(size_t done, size_t total, const std::string& name)>;
HarvestScan scanInstalledGamesSdk(const HarvestProgress& progress, const std::atomic<bool>* cancel);

// Сохранить кандидата: открыть exefs игры-источника (первая игра кандидата) и скопировать её sdk в
// sdmc:/switch/<APP>/sdk/<папка>/sdk + source.json (mk::harvest::save_candidate). Игра-источник не открылась —
// пробуются остальные игры с тем же файлом.
mk::harvest::SaveResult saveHarvested(const mk::harvest::Candidate& c);

}  // namespace mk::plat
