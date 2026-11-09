// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.cpp (RecyclerDataSource списка игр,
// фоновая загрузка brls::async → brls::sync с токеном отмены)
#include "ui/games_activity.hpp"

#include "app_model.hpp"
#include "ui/game_activity.hpp"
#include "ui/language_activity.hpp"
#include "ui/settings_activity.hpp"
#include "ui/text_sheet.hpp"

namespace mk::ui {

using mk::app::GameRow;
using mk::plat::SdkNeed;

// ---------------- ячейка ----------------

GameCell::GameCell() {
    this->inflateFromXMLRes("xml/game_cell.xml");
    translateIds(this, {"games/badge_ours", "games/badge_mods"});
}

GameCell* GameCell::create() { return new GameCell(); }

void GameCell::fill(const GameRow& r) {
    name->setText(r.name);
    std::string s = r.version.empty() ? r.tid : trf("games/version_tid", r.version, r.tid);
    sub->setText(s);
    if (!r.icon.empty()) {
        icon->setImageFromMem(r.icon.data(), static_cast<int>(r.icon.size()));
    } else {
        icon->setImageFromRes("img/cover_blank.png");  // из TorrentShopNX, см. resources/CREDITS.md
    }
    badgeOurs->setVisibility(r.ourInstalled ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    badgeMods->setVisibility(r.modsAvailable ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    switch (r.sdkNeed) {
        case SdkNeed::Needed:
            badgeSdk->setVisibility(brls::Visibility::VISIBLE);
            badgeSdk->setBackgroundColor(colWarn());
            badgeSdkText->setText(tr("games/badge_sdk_needed"));
            break;
        case SdkNeed::SwappedOk:
            badgeSdk->setVisibility(brls::Visibility::VISIBLE);
            badgeSdk->setBackgroundColor(colOk());
            badgeSdkText->setText(tr("games/badge_sdk_swapped"));
            break;
        case SdkNeed::SwappedTooNew:
        case SdkNeed::SwappedUnknown:
            badgeSdk->setVisibility(brls::Visibility::VISIBLE);
            badgeSdk->setBackgroundColor(colBad());
            badgeSdkText->setText(tr("games/badge_sdk_check"));
            break;
        default:
            badgeSdk->setVisibility(brls::Visibility::GONE);
            break;
    }
    sdkRight->setText(r.gameSdk.empty() ? "" : trf("games/sdk_right", r.gameSdk));
}

// ---------------- источник данных ----------------

class GamesDataSource : public brls::RecyclerDataSource {
public:
    explicit GamesDataSource(GamesActivity* a) : a_(a) {}
    int numberOfRows(brls::RecyclerFrame*, int) override { return static_cast<int>(a_->rows_.size()); }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = dynamic_cast<GameCell*>(recycler->dequeueReusableCell("Game"));
        if (cell && index.row >= 0 && static_cast<size_t>(index.row) < a_->rows_.size()) cell->fill(a_->rows_[index.row]);
        return cell;
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override { return 92; }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (index.row < 0 || static_cast<size_t>(index.row) >= a_->rows_.size()) return;
        GamesActivity* owner = a_;
        AliveToken alive = owner->alive_;
        brls::Application::pushActivity(new GameActivity(a_->rows_[index.row], [owner, alive](const GameRow& r) {
            if (*alive) owner->updateRow(r);
        }));
    }

private:
    GamesActivity* a_;
};

// ---------------- экран ----------------

GamesActivity::GamesActivity(bool askLanguage) : askLanguage_(askLanguage) {}

GamesActivity::~GamesActivity() { *alive_ = false; }

void GamesActivity::onContentAvailable() {
    list->registerCell("Game", []() { return GameCell::create(); });
    list->setDataSource(new GamesDataSource(this));
    // Фокус должен всегда стоять на живом виде этого экрана. Пока список грузится, видимых строк нет, и без опоры
    // фокус оставался на строке экрана поверх (выбор языка, диалог). Borealis при его закрытии не переводит фокус,
    // если стек фокуса пуст, — указатель висит на удалённом виде, и следующий giveFocus(list) зовёт onFocusLost
    // у мёртвого объекта: вылет при первом запуске после выбора языка.
    statusBox->setFocusable(true);
    statusBox->setHideHighlight(true);
    statusBox->setHideClickAnimation(true);
    if (askLanguage_) {
        // Первый запуск: выбор языка поверх списка; список строится уже на выбранном языке.
        showStatus("");
        AliveToken alive = alive_;
        brls::sync([this, alive] {
            brls::Application::pushActivity(new LanguageActivity([this, alive] {
                if (!*alive) return;
                askLanguage_ = false;
                applyTexts();
                reload();
            }));
        });
        return;
    }
    applyTexts();
    reload();
}

void GamesActivity::applyTexts() {
    setFrameTitle(this, "games/title");
    // B на главном экране — выход с вопросом (как у borealis), но тексты — нашим переводом.
    registerAction(tr("hint/back"), brls::ControllerButton::BUTTON_B, [](brls::View*) {
        auto* dlg = makeShortDialog(tr("common/exit_confirm"));
        dlg->addButton(tr("common/cancel"), [] {});
        dlg->addButton(tr("common/ok_short"), [] { brls::Application::quit(); });
        dlg->open();
        return true;
    });
    registerAction(tr("hint/settings"), brls::ControllerButton::BUTTON_X, [](brls::View*) {
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });
    registerAction(tr("hint/exit"), brls::ControllerButton::BUTTON_START, [](brls::View*) {
        brls::Application::quit();
        return true;
    });
    registerAction(tr("hint/refresh"), brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
        reload();
        return true;
    });

    const auto ci = mk::app::Model::instance().console();
    consoleLine->setText(trf("games/console_line", ci.firmware.valid ? ci.firmwareDisplay : tr("common/unknown"),
                             ci.amsKnown ? ci.atmosphere.toString() : tr("common/unknown")));
}

void GamesActivity::showStatus(const std::string& text) {
    statusText->setText(text);
    statusBox->setVisibility(brls::Visibility::VISIBLE);
    list->setVisibility(brls::Visibility::GONE);
    brls::Application::giveFocus(statusBox);
}

void GamesActivity::reload() {
    if (loading_) return;
    loading_ = true;
    showStatus(tr("games/loading"));
    countLine->setText("");
    AliveToken alive = alive_;
    brls::async([this, alive]() {
        auto& m = mk::app::Model::instance();
        std::vector<GameRow> rows;
        const bool ok = m.servicesOk();
        if (ok) {
            rows = m.loadGames();
            (void)m.sdkList(true);  // обновить и список sdk — он нужен на экране игры
        }
        brls::sync([this, alive, ok, rows = std::move(rows)]() mutable {
            if (!*alive) return;
            loading_ = false;
            rows_ = std::move(rows);
            if (!ok) {
                showStatus(tr("games/services_failed"));
                return;
            }
            if (rows_.empty()) {
                showStatus(tr("games/empty"));
                return;
            }
            countLine->setText(trf("games/count", std::to_string(rows_.size())));
            statusBox->setVisibility(brls::Visibility::GONE);
            list->setVisibility(brls::Visibility::VISIBLE);
            list->reloadData();
            brls::Application::giveFocus(list);
        });
    });
}

void GamesActivity::updateRow(const GameRow& row) {
    for (auto& r : rows_) {
        if (r.id == row.id) {
            r.ourInstalled = row.ourInstalled;
            r.sdkNeed = row.sdkNeed;
            if (!row.gameSdk.empty()) r.gameSdk = row.gameSdk;
        }
    }
    if (!rows_.empty()) list->reloadData();
}

}  // namespace mk::ui
