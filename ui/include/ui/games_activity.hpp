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
    explicit GamesActivity(bool askLanguage = false);
    ~GamesActivity() override;
    void onContentAvailable() override;

    // Вызывается экраном игры после установки/удаления: обновить строку.
    void updateRow(const mk::app::GameRow& row);

private:
    void applyTexts();   // заголовок, подсказки кнопок, строка консоли — текущим языком
    void reload();
    void showStatus(const std::string& text);
    bool askLanguage_ = false;

    BRLS_BIND(brls::Label, consoleLine, "consoleLine");
    BRLS_BIND(brls::Label, countLine, "countLine");
    BRLS_BIND(brls::Box, statusBox, "statusBox");
    BRLS_BIND(brls::Label, statusText, "statusText");
    BRLS_BIND(brls::RecyclerFrame, list, "list");

    std::vector<mk::app::GameRow> rows_;
    AliveToken alive_ = makeAlive();
    bool loading_ = false;

    friend class GamesDataSource;
};

}  // namespace mk::ui
