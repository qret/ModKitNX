// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — модель приложения: собирает данные из platform/ и ядра (через core_bridge) для экранов.
// Все методы, кроме помеченных «быстро», блокирующие (IPC, карта) — звать из brls::async, не из UI-потока.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "app_types.hpp"
#include "harvest_types.hpp"
#include "platform/console_info.hpp"
#include "platform/sdk_scan.hpp"

namespace mk::app {

class Model {
public:
    static Model& instance();

    // Один раз при старте (режим тайтла): службы, журнал уже открыт.
    void startup();
    void shutdown();
    bool servicesOk() const { return servicesOk_; }

    plat::ConsoleInfo console();  // быстро после первого вызова

    // Список игр (ns + NACP + заголовок NCA для значка sdk). Сортировка по названию.
    std::vector<GameRow> loadGames();

    // sdk из двух папок без повторов. reload=false — из кэша.
    std::vector<SdkLine> sdkList(bool reload = false);

    // Всё об одной игре: ncm, заголовок NCA, exefs, ядро, моды, блок sdk.
    GameDetails loadDetails(const GameRow& row);

    // План и исполнение. Экран плана держит PlanView; apply исполняет последний построенный план.
    // debugBuild — «Отладочная сборка»: ядро ставит диагностический вариант модуля (autofix-diag / ранний).
    PlanView plan(OpKind op, const GameDetails& d, const std::string& modPath, const std::string& sdkPath,
                  bool debugBuild);
    ApplyView apply(const GameDetails& d, const std::function<void(size_t, size_t, const PlanItem&)>& progress);

    // Паспорт консоли в sdmc:/switch/<APP>/console-passport.txt.
    bool writeConsolePassport(size_t gameCount);

    // «Собрать логи для отправки»: папка и .zip в sdmc:/switch/<APP>/logs/ (блокирующий).
    LogsView collectLogs(const GameDetails& d);

    // «Найти sdk в других играх» (блокирующие): пройти установленные игры (только файл sdk каждой; кэш —
    // пересканируются новые и обновлённые) и выбрать подходящие прошивке; progress(проверено, всего, игра).
    // cancel — остановиться после текущей игры. Ничего не скачивается.
    HarvestView findSdkInGames(const std::function<void(size_t, size_t, const std::string&)>& progress,
                               const std::atomic<bool>* cancel);
    // Сохранить строку index последнего поиска в sdmc:/switch/<APP>/sdk/<папка>/sdk + source.json.
    HarvestSaveView saveSdkFromGames(size_t index);

private:
    Model() = default;
    void scanSdkLocked();

    bool servicesOk_ = false;
    std::mutex sdkMtx_;
    bool sdkScanned_ = false;
    std::vector<plat::FoundSdk> sdk_;
    std::mutex harvestMtx_;
    std::vector<HarvestLine> harvest_;   // строки последнего поиска sdk в играх
};

}  // namespace mk::app
