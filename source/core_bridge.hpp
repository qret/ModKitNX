// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — ЕДИНСТВЕННАЯ точка вызова операций ядра (core/include/mk/*.hpp) для экранов.
// Этот заголовок от ядра не зависит: экраны и модель видят только типы mk::app. Исключения mk::Error
// ловятся внутри и превращаются в коды/тексты ошибок.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app_types.hpp"

namespace mk::bridge {

struct GameCtx {
    uint64_t titleId = 0;
    std::string tid;          // 16 hex заглавными
    std::string displayVersion;
    bool exefsOpen = false;   // exefs игры смонтирована как mkexefs:/
};

struct SdkProbe {
    bool isNso = false;
    bool isNnSdk = false;
    std::string moduleName;
    std::string mwVersion;    // «19.3.13» или ""
};

// Опознание NSO по содержимому (exefs/sdk игры, подменённая sdk) — mk::identify_sdk.
bool identifySdk(const std::string& path, SdkProbe& out);

// Состояние игры: квитанция, ZBIC, собственная sdk игры, лежащий exefs/sdk. Заполняет поля d.
bool inspectGame(const GameCtx& g, app::GameDetails& d);

// Блок «Что исправит модуль» (d.fixes): импорты main/subsdk игры (exefs должна быть смонтирована) × sdk.
// currentSdkPath — sdk, которая у игры сейчас (подменённая на карте или родная mkexefs:/sdk), "" — нет;
// candidates — найденные sdk (путь, sha256) по порядку SdkLine. Сторона sdk кэшируется по пути и sha256.
void forecastFixes(const GameCtx& g, const std::string& currentSdkPath, bool currentIsSwap,
                   const std::vector<std::pair<std::string, std::string>>& candidates, app::GameDetails& d);

// Моды к игре: папки в sdmc:/switch/<APP>/mods/ (дерево мода опознаёт ядро) + то, что уже лежит в
// atmosphere/contents/<TID> (только для показа).
std::vector<app::ModLine> findMods(const GameCtx& g);

// План операции (ничего не пишет). Последний план запоминается внутри моста и исполняется applyPending.
// unpackZbic — решение mk::zbic_need; debugBuild — диагностический вариант модуля из описи romfs.
app::PlanView makePlan(app::OpKind op, const GameCtx& g, const std::string& modPath, const std::string& sdkPath,
                       bool unpackZbic, bool debugBuild);

using ProgressFn = std::function<void(size_t index, size_t total, const app::PlanItem& item)>;
app::ApplyView applyPending(const GameCtx& g, const ProgressFn& progress);

// «Собрать логи для отправки» (mk::logs): отчёты diag/gfx игры, последний отчёт Atmosphère о падении, паспорт,
// квитанция -> sdmc:/switch/<APP>/logs/<Игра> v<версия>/ и .zip рядом. write=false — только список (предпросмотр).
// appTexts — тексты приложения в сборку ({имя, текст}: журнал чтения exefs «exefs-read.txt»).
app::LogsView collectLogs(const GameCtx& g, const std::string& gameName, const std::string& passportText, bool write,
                          const std::vector<std::pair<std::string, std::string>>& appTexts = {});

}  // namespace mk::bridge
