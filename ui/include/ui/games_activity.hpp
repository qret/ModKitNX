// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.hpp (LibraryRowCell, список игр на
// RecyclerFrame с привязкой BRLS_BIND)
//
// Главный экран — список установленных игр: иконка, название, версия, значки «мод установлен» и «нужна подмена sdk».
// A — открыть игру, X — настройки и «О программе», Y — обновить список, + — выход.
#pragma once

#include <borealis.hpp>

#include <vector>

#include "app_types.hpp"
#include "ui/common.hpp"

namespace mk::ui {

class GameCell : public brls::RecyclerCell {
public:
    GameCell();
    static GameCell* create();
    void fill(const mk::app::GameRow& row);

    BRLS_BIND(brls::Image, icon, "icon");
    BRLS_BIND(brls::Label, name, "name");
    BRLS_BIND(brls::Label, sub, "sub");
    BRLS_BIND(brls::Box, badgeOurs, "badgeOurs");
    BRLS_BIND(brls::Box, badgeSdk, "badgeSdk");
    BRLS_BIND(brls::Label, badgeSdkText, "badgeSdkText");
    BRLS_BIND(brls::Box, badgeMods, "badgeMods");
    BRLS_BIND(brls::Label, sdkRight, "sdkRight");
};

class GamesActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("games_activity.xml");
    // askLanguage — первый запуск: сначала экран выбора языка (ui/language_activity), список — после выбора.
    // startupPrompts — вопросы запуска (source/main.cpp): один раз «Создать ярлык на домашнем экране?» (после выбора
    // языка или при первом запуске новой версии), иначе — тихая проверка обновления (не при первом запуске).
    // Предпросмотр зовёт без них, чтобы окна не легли на снимки.
    explicit GamesActivity(bool askLanguage = false, bool startupPrompts = false);
    ~GamesActivity() override;
    void onContentAvailable() override;
    void onResume() override;

    // Вызывается экраном игры: при открытии страницы (focus = false) и после операции (focus = true) — обновить
    // метки строки. Строка не изменилась и фокус ставить не нужно — список не трогается. Список под другим экраном —
    // применится при возврате (onResume), фокус — на ту же игру.
    void updateRow(const mk::app::GameRow& row, bool focus);

private:
    void applyTexts();   // заголовок, подсказки кнопок, строка консоли — текущим языком
    // bypassCache — Y «Обновить»: всё живьём, мимо кэша (и перезапись кэша); фокус остаётся на той же игре.
    void reload(bool bypassCache = false);
    // Перестроить ячейки и поставить фокус на игру id (её нет — на первую). Borealis не умеет обновить одну ячейку:
    // reloadData снимает все ячейки (фокус оставался бы на снятом объекте — рамка на чужой игре), поэтому
    // фокус ставится заново на строку по id.
    void applyRows(uint64_t focusId);
    uint64_t focusedId() const;   // игра в фокусе (0 — фокус не на строке списка)
    void showStatus(const std::string& text);
    bool askLanguage_ = false;
    bool startupPrompts_ = false;

    BRLS_BIND(brls::Label, consoleLine, "consoleLine");
    BRLS_BIND(brls::Label, countLine, "countLine");
    BRLS_BIND(brls::Box, statusBox, "statusBox");
    BRLS_BIND(brls::Label, statusText, "statusText");
    BRLS_BIND(brls::RecyclerFrame, list, "list");

    std::vector<mk::app::GameRow> rows_;
    AliveToken alive_ = makeAlive();
    bool loading_ = false;
    bool pendingApply_ = false;     // метки строки изменились, пока список был под другим экраном
    uint64_t pendingFocus_ = 0;     // на какую игру вернуть фокус при применении

    friend class GamesDataSource;
};

}  // namespace mk::ui
