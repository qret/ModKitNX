// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран игры. Сверху вниз, один вопрос за раз:
//   1. «Что есть» — версия, обновление, требуемая прошивка, ZBIC, установлено ли наше, как прочитан код игры;
//   2. «sdk» — что требует игра (NCA и своя sdk из exefs), что у неё сейчас на консоли, прошивка и Atmosphère,
//      вывод простыми словами и выбор sdk прямо под вопросом; кнопки «Подменить sdk» / «Убрать подмену»;
//   2а. «Что исправит модуль» — прогноз ядра для пары «код игры × sdk, которая будет у игры» (выбранная для
//      подмены, иначе текущая); меняется вместе с выбором sdk;
//   3. «Моды» — выбор мода прямо под вопросом;
//   4. «Установить» / «Удалить установленное».
#pragma once

#include <borealis.hpp>

#include <functional>
#include <vector>

#include "app_types.hpp"
#include "game_actions.hpp"
#include "ui/diag_activity.hpp"
#include "ui/common.hpp"

namespace mk::ui {

class GameActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("game_activity.xml");
    GameActivity(const mk::app::GameRow& row, std::function<void(const mk::app::GameRow&)> onRowChanged);
    ~GameActivity() override;
    void onContentAvailable() override;

private:
    void reload();
    void build();
    // Очистить тело экрана, не удаляя вид с фокусом: фокус сначала переводится на опору anchor_ (она не удаляется).
    // Деструктор вида Borealis при удалении сфокусированного вида зовёт giveFocus(nullptr), а тот ничего не делает —
    // указатель фокуса оставался бы на удалённой кнопке (та же ловушка, что вылет после выбора языка).
    void clearBody();
    // Успешная операция: сразу к списку игр (на ту же игру), значки строки обновятся в фоне
    // (после «Готово» не ждать перечитывания страницы игры).
    void returnToList(bool removal);
    void buildHeader(brls::Box* body);
    void buildState(brls::Box* body);
    void buildSdk(brls::Box* body);
    void buildFixes(brls::Box* body);
    void refreshFixes();
    void addGaps(const mk::app::FixForecastView& f);   // чего модуль не восполнит в выбранной sdk
    void buildMods(brls::Box* body);
    void buildActions(brls::Box* body);
    // Кнопки «Подменить sdk» / «Удалить подмену», «Установить» / «Удалить установленное» / «Переустановить»,
    // «Отчёт отладки», «Собрать логи» — по mk::app::decideGameActions; перестраиваются при выборе sdk, мода и флажка.
    void refreshActions();
    mk::app::GameActionsInput actionsInput() const;
    void startOperation(mk::app::OpKind op, bool withSdk = true);   // withSdk = false — без подмены sdk
    std::string selectedSdkPath() const;
    std::string selectedModPath() const;

    BRLS_BIND(brls::Box, body, "body");
    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");

    mk::app::GameRow row_;
    mk::app::GameDetails d_;
    std::vector<mk::app::SdkLine> sdk_;
    bool loaded_ = false;
    int sdkChoice_ = -1;   // -1 — «не подменять»
    int modChoice_ = -1;   // -1 — «без мода»
    bool debug_ = false;   // «Отладочная сборка»: ставить диагностический вариант модуля
    std::vector<ChoiceRow*> sdkRows_;
    std::vector<ChoiceRow*> modRows_;
    brls::Box* fixesBox_ = nullptr;
    brls::Box* sdkButtons_ = nullptr;   // «Подменить sdk» / «Удалить подмену» в блоке sdk
    brls::Box* sdkChoiceBox_ = nullptr;   // вопрос и выбор sdk (скрывается, когда подмена не нужна)
    brls::View* debugRow_ = nullptr;      // флажок «Отладочная сборка»
    brls::Box* actionsBox_ = nullptr;   // кнопки действий внизу страницы
    ReportsInfo reports_;               // отчёты этой игры на карте (findReports, в фоне)   // содержимое блока «Что исправит модуль» (перестраивается при выборе sdk)
    brls::View* firstFocus_ = nullptr;
    brls::Box* anchor_ = nullptr;   // невидимая фокусируемая опора — всегда первый ребёнок body
    std::function<void(const mk::app::GameRow&)> onRowChanged_;
    AliveToken alive_ = makeAlive();
};

}  // namespace mk::ui
