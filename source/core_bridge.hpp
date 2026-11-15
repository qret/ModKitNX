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
    std::string sha256;       // содержимое файла (hex)
};

// Опознание NSO по содержимому (подменённая sdk) — mk::identify_sdk.
bool identifySdk(const std::string& path, SdkProbe& out);

// Ключи кэша проверки игры sdmc:/switch/<APP>/cache/game/<TID>.json (mk::cache::GameCheck).
struct CheckKeys {
    std::string key;          // признак содержимого игры (plat::contentKeyOf); пусто — кэш не читается и не пишется
    std::string fw, ams;      // прошивка и Atmosphère — от них зависит, как открывается exefs
    bool useCache = true;     // false — мимо кэша (Y «Обновить», после операции); свежий результат перезаписывает кэш
};

// Проверка игры для экрана (ТОЛЬКО показ; план и запись кэш не читают). Квитанция — живьём; ZBIC, своя sdk игры и
// импорты — из кэша или из exefs: openExefs монтирует её как mkexefs:/ только при промахе (закрывает вызывающий).
// «Что исправит модуль»: текущая sdk — подмена swapPath (sha256 swapSha) или, без подмены, своя sdk игры (из того
// же чтения, что импорты); кандидаты — найденные sdk (путь, sha256) по порядку SdkLine. Стороны sdk — по sha256
// (в кэше игры и в памяти). Ошибки и временные сбои в кэш не пишутся. Заполняет d: exefsOpened, exefs, coreOk,
// zbic, zbicFiles, sdk.exefsSdk, квитанцию, fixes, checkFromCache.
void checkGame(const GameCtx& g, const CheckKeys& k, const std::function<bool(plat::ExefsReport&)>& openExefs,
               const std::string& swapPath, const std::string& swapSha,
               const std::vector<std::pair<std::string, std::string>>& candidates, app::GameDetails& d);
// Сбросить кэш проверки игры (после любой операции с ней). false — файл не убрался (будет промах по app/ключу
// только при их смене; запись в журнал).
bool dropGameCheck(const std::string& tid);
// «Сбросить кэш»: стороны sdk в памяти моста.
void clearMemoryCache();

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
